#include "Runtime/ElementFireDomain.h"
#include "Runtime/ElementFireShardedMap.h"

#include "AbilitySystemComponent.h"
#include "Async/Async.h"
#include "BuildingWorldSubsystem.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "Combustion/BuildCombustionCatalog.h"
#include "Combustion/WorldObjectCombustionCatalog.h"
#include "Definition/BuildingDefinition.h"
#include "Definition/WorldDestructionDefinition.h"
#include "Effects/ElementCharacterBurningEffect.h"
#include "ElementSimulationSubsystem.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Fire/ElementFireRuleSet.h"
#include "Fire/ElementFireWorldObjectProjection.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Runtime/ElementAuthorityExecution.h"
#include "Runtime/ElementFireBuildingState.h"
#include "Runtime/ElementFireRuntimeTypes.h"
#include "Shape/BuildShapeTypes.h"
#include "Shape/WorldObjectShapeTypes.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Storage/ElementFirePersistenceTypes.h"
#include "Storage/ElementWorldStorageAdapter.h"
#include "Tree/SettlementTreeTypes.h"
#include "Tree/SettlementTreeWorldSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Visual/ElementVisualJournal.h"
#include "WorldObjects/CharcoalWorldObjectDefinition.h"
#include "WorldDestructionAuthorityService.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

using UE::ElementSandbox::ElementGameplay::Private::FElementFireBuildingState;

namespace
{
	constexpr double FireConeUnscaledHalfHeightCentimeters = 50.0;
	const FName FireVisualKind(TEXT("Fire.Flame"));
	const FName FireVisualDefinition(TEXT("Element.Fire.Flame"));
	const FVector BuildingFlameScale(0.65, 0.65, 1.20);
	const FVector TreeFlameScale(1.40, 1.40, 2.20);
	constexpr float BurnPresentationPrecision = 1.0f / 255.0f;

	uint64 AdvanceNonZero(const uint64 Value)
	{
		return Value == MAX_uint64 ? 1 : Value + 1;
	}

	uint32 AdvancePersistentRevision(const uint32 Value)
	{
		// WorldStorage Revision 不允许倒退。实际项目寿命内不可能耗尽；到达上限时保持饱和。
		return Value == MAX_uint32 ? MAX_uint32 : Value + 1;
	}

	int64 GetAuthorityTime(const UWorldStorageSubsystem* Storage, const UWorld* World)
	{
		if (Storage) return FMath::Max<int64>(0, Storage->GetWorldSimulationTimeMilliseconds());
		return World ? FMath::Max<int64>(0, FMath::RoundToInt64(World->GetTimeSeconds() * 1000.0)) : 0;
	}

	float ComputeBurnPresentationAmount(
		const EFireCombustionPhase Phase,
		const int64 BurnStartMilliseconds,
		const int64 BurnEndMilliseconds,
		const double CurrentTimeMilliseconds)
	{
		if (Phase == EFireCombustionPhase::BurnedOut) return 1.0f;
		if (Phase != EFireCombustionPhase::Burning
			|| BurnStartMilliseconds < 0 || BurnEndMilliseconds <= BurnStartMilliseconds
			|| !FMath::IsFinite(CurrentTimeMilliseconds))
		{
			return 0.0f;
		}
		return FMath::Clamp(static_cast<float>(
			(CurrentTimeMilliseconds - static_cast<double>(BurnStartMilliseconds))
			/ static_cast<double>(BurnEndMilliseconds - BurnStartMilliseconds)), 0.0f, 1.0f);
	}

	FElementTargetKey MakeTargetKey(
		const FWorldEntityId WorldId,
		const FBuildEntityHandle Entity)
	{
		FElementTargetKey Key;
		Key.Domain = EElementTargetDomain::Building;
		Key.WorldEntityId = WorldId;
		Key.RegistryId = Entity.GetRegistryId();
		Key.Slot = Entity.GetIndex();
		Key.Generation = Entity.GetGeneration();
		Key.PartId = 0;
		return Key;
	}

	FElementTargetKey MakeTargetKey(
		const FWorldEntityId WorldId,
		const FWorldObjectEntityHandle Entity)
	{
		FElementTargetKey Key;
		Key.Domain = EElementTargetDomain::WorldObject;
		Key.WorldEntityId = WorldId;
		Key.RegistryId = Entity.GetRegistryId();
		Key.Slot = Entity.GetSlot();
		Key.Generation = Entity.GetGeneration();
		Key.PartId = 0;
		return Key;
	}

	FElementTargetKey MakeTargetKey(const FCharacterSnapshotHandle Handle)
	{
		FElementTargetKey Key;
		Key.Domain = EElementTargetDomain::Character;
		Key.RegistryId = Handle.GetRegistryId();
		Key.Slot = Handle.GetSlot();
		Key.Generation = Handle.GetGeneration();
		Key.PartId = 0;
		return Key;
	}

	uint64 MakeBuildChildKey(const FBuildShapeRef& Ref)
	{
		return (static_cast<uint64>(static_cast<uint32>(Ref.PartId)) << 16)
			| static_cast<uint64>(Ref.ShapeId);
	}

	double MaximumScale(const FTransform& Transform)
	{
		return Transform.GetScale3D().GetAbs().GetMax();
	}

	FElementShape ConvertBuildGeometryToWorld(const FBuildShapeInstanceSnapshot& Snapshot)
	{
		const FBuildPartShapeDefinition& Shape = Snapshot.LocalGeometry;
		const FTransform& Transform = Snapshot.WorldTransform;
		const double Scale = MaximumScale(Transform);
		const FVector Center = Transform.TransformPosition(Shape.Center);
		switch (Shape.Kind)
		{
		case EBuildPartShapeKind::Sphere:
			return FElementShape::MakeSphere(Center, Shape.Radius * Scale);
		case EBuildPartShapeKind::Capsule:
			return FElementShape::MakeCapsule(
				Center,
				Transform.TransformVectorNoScale(Shape.CapsuleAxis).GetSafeNormal(),
				Shape.Radius * Scale,
				Shape.CapsuleSegmentHalfLength * Scale);
		case EBuildPartShapeKind::Obb:
			return FElementShape::MakeBox(
				Center,
				(Transform.GetRotation() * Shape.Rotation).GetNormalized(),
				Shape.HalfExtents * Transform.GetScale3D().GetAbs());
		default:
			return {};
		}
	}

	FElementShape ConvertWorldObjectGeometry(const FWorldObjectShapeDefinition& Shape)
	{
		switch (Shape.Kind)
		{
		case EWorldObjectShapeKind::Sphere:
			return FElementShape::MakeSphere(Shape.Center, Shape.Radius);
		case EWorldObjectShapeKind::Capsule:
			return FElementShape::MakeCapsule(
				Shape.Center, Shape.CapsuleAxis, Shape.Radius, Shape.CapsuleSegmentHalfLength);
		case EWorldObjectShapeKind::Obb:
			return FElementShape::MakeBox(Shape.Center, Shape.Rotation, Shape.HalfExtents);
		default:
			return {};
		}
	}

	FElementCompoundShape MakeCharacterShape(const FCharacterQuerySnapshot& Snapshot)
	{
		FElementCompoundShape Result;
		Result.WorldTransform = Snapshot.WorldTransform;
		const double Scale = FMath::Max(MaximumScale(Snapshot.WorldTransform), UE_DOUBLE_SMALL_NUMBER);
		const FVector LocalCenter = Snapshot.WorldTransform.InverseTransformPosition(Snapshot.Capsule.Center);
		const FVector LocalAxis = Snapshot.WorldTransform.InverseTransformVectorNoScale(
			Snapshot.Capsule.Axis).GetSafeNormal();
		Result.Shapes.Add(FElementShape::MakeCapsule(
			LocalCenter,
			LocalAxis,
			Snapshot.Capsule.Radius / Scale,
			Snapshot.Capsule.GetSegmentHalfLength() / Scale));
		return Result;
	}

	bool RemoveAllCharacterBurningEffects(UAbilitySystemComponent& AbilitySystem)
	{
		bool bSucceeded = true;
		for (const FActiveGameplayEffectHandle Handle : AbilitySystem.GetActiveEffects(FGameplayEffectQuery()))
		{
			const FActiveGameplayEffect* Effect = AbilitySystem.GetActiveGameplayEffect(Handle);
			if (Effect && Effect->Spec.Def && Effect->Spec.Def->IsA<UElementCharacterBurningEffect>())
			{
				bSucceeded = AbilitySystem.RemoveActiveGameplayEffect(Handle) && bSucceeded;
			}
		}
		return bSucceeded;
	}

	EFireCombustionPhase ReadProjectionPhase(const FElementProjectionCommand& Command)
	{
		return Command.Payload.Count >= 1
			? static_cast<EFireCombustionPhase>(FMath::Clamp(
				FMath::RoundToInt(Command.Payload.Values[0]),
				0,
				static_cast<int32>(EFireCombustionPhase::BurnedOut)))
			: EFireCombustionPhase::Cold;
	}

	const FWorldDestructionDefinition& GetBurnoutProducts()
	{
		static const FWorldDestructionDefinition Products = []
		{
			FWorldDestructionDefinition Result;
			// MaxDurability 只负责让共享产品配方通过完整校验；燃尽转换不读取或写入伤害。
			Result.MaxDurability = 1.0f;
			Result.ProductClass = UCharcoalWorldObjectDefinition::StaticClass();
			Result.MinimumProductCount = 3;
			Result.MaximumProductCount = 6;
			Result.UniformScaleRange = FVector2D(0.85, 1.15);
			Result.SpawnOffsetExtent = FVector(60.0, 60.0, 25.0);
			Result.HorizontalSpeedRange = FVector2D(5.0, 25.0);
			Result.UpwardSpeedRange = FVector2D(0.0, 30.0);
			Result.AngularSpeedRange = FVector2D(10.0, 45.0);
			Result.ProductMassKg = 0.8f;
			return Result;
		}();
		return Products;
	}
}

class FElementFireDomainData final
{
public:
	struct FHostRecord final
	{
		FElementTargetKey Target;
		EElementFireTargetProfile Profile = EElementFireTargetProfile::None;
		FWorldEntityId WorldEntityId;
		FBuildEntityHandle Building;
		FWorldObjectEntityHandle WorldObject;
		FCharacterSnapshotHandle Character;
		FName DefinitionId = NAME_None;
		TMap<uint64, FElementShape> Shapes;
		FTransform WorldTransform = FTransform::Identity;
		uint64 SnapshotRevision = 0;
		int64 LastEffectiveTimeMilliseconds = 0;
		int32 BurnCustomDataIndex = INDEX_NONE;
		uint64 LastProjectionRevision = 0;
		bool bFireInteractionActive = false;
		bool bPublished = false;

		FElementCompoundShape MakeCompoundShape() const
		{
			FElementCompoundShape Result;
			Result.WorldTransform = WorldTransform;
			TArray<uint64> Keys;
			Shapes.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const uint64 Key : Keys)
			{
				if (const FElementShape* Shape = Shapes.Find(Key)) Result.Shapes.Add(*Shape);
			}
			return Result;
		}
	};

	struct FFixedSource final
	{
		FBuildEntityHandle Host;
		FElementEntityHandle Element;
	};

	struct FPendingBuildingSnapshotBatch final
	{
		TSharedPtr<const FBuildQuerySnapshotBatch, ESPMode::ThreadSafe> Batch;
		int32 ChangeCursor = 0;
		FBuildEntityHandle ActiveEntity;
		int64 ActiveEntityEffectiveTimeMilliseconds = 0;
		bool bActiveEntityDirty = false;
	};

	struct FBurnPresentation final
	{
		EFireCombustionPhase Phase = EFireCombustionPhase::Cold;
		int64 BurnStartMilliseconds = 0;
		int64 BurnEndMilliseconds = 0;
		int64 AuthorityReferenceMilliseconds = 0;
		double LocalReferenceSeconds = 0.0;
		float LastAppliedAmount = -1.0f;
	};

	struct FRuntimeSource final
	{
		FElementRuntimeFireSourceHandle Handle;
		FElementEntityHandle Element;
		int64 ExpireTimeMilliseconds = 0;
		uint64 Token = 0;
	};

	struct FRuntimeSourceExpiry final
	{
		int64 DueTimeMilliseconds = 0;
		FElementRuntimeFireSourceHandle Handle;
		uint64 Token = 0;
	};

	struct FPersistentBinding final
	{
		FWorldEntityId ElementId;
		FWorldEntityId HostId;
		FElementTargetKey Target;
		FElementEntityHandle Element;
		FWorldChunkCoord HomeChunk;
		uint32 StateRevision = 1;
		bool bHasFireSource = false;
	};

	struct FExpiryPredicate final
	{
		bool operator()(const FRuntimeSourceExpiry& Left, const FRuntimeSourceExpiry& Right) const
		{
			return Left.DueTimeMilliseconds < Right.DueTimeMilliseconds;
		}
	};

	explicit FElementFireDomainData(
		FElementFireDomain& InDomainOwner,
		UElementGameplayWorldSubsystem& InOwner)
		: DomainOwner(&InDomainOwner), Owner(&InOwner)
	{
	}

#include "Runtime/ElementFireDomainLifecycle.inl"
#include "Runtime/ElementFireHostSnapshotBridge.inl"
#include "Runtime/ElementFireProjectionBridge.inl"
#include "Runtime/ElementFirePersistenceBridge.inl"

	FElementFireDomain* DomainOwner = nullptr;
	TWeakObjectPtr<UElementGameplayWorldSubsystem> Owner;
	TWeakObjectPtr<UElementSimulationSubsystem> Simulation;
	TWeakObjectPtr<UBuildingWorldSubsystem> Buildings;
	TWeakObjectPtr<UWorldObjectWorldSubsystem> WorldObjects;
	TWeakObjectPtr<UCharacterQuerySnapshotSubsystem> Characters;
	TWeakObjectPtr<USettlementTreeWorldSubsystem> Trees;
	TWeakObjectPtr<UWorldStorageSubsystem> Storage;
	TStrongObjectPtr<UElementFireRuleSet> RuleAsset;
	TSharedPtr<IWorldStorageDomainAdapter> StorageAdapter;
	FFireRuleSnapshot Rules;
	FElementAuthorityExecution* Execution = nullptr;

	TElementFireShardedMap<FElementTargetKey, FHostRecord> Hosts;
	TElementFireShardedMap<FBuildEntityHandle, FElementTargetKey> BuildingTargets;
	TElementFireShardedMap<FWorldObjectEntityHandle, FElementTargetKey> WorldObjectTargets;
	TElementFireShardedMap<FCharacterSnapshotHandle, FElementTargetKey> CharacterTargets;
	TMap<FWorldEntityId, FFixedSource> FixedSources;
	TMap<FName, FBuildFireShapeRoute> BuildingFireShapeRoutes;
	TMap<FWorldEntityId, FPersistentBinding> PersistentBindings;
	TMap<FElementTargetKey, FWorldEntityId> PersistentIdByTarget;
	TElementFireShardedMap<FWorldEntityId, FElementTargetKey> TargetByHostId;
	TSet<FElementTargetKey> PendingPersistenceDirty;
	TMap<FElementTargetKey, uint64> PendingBurnoutConversions;
	TMap<FElementRuntimeFireSourceHandle, FRuntimeSource> RuntimeSources;
	TArray<FRuntimeSourceExpiry> RuntimeSourceExpiries;
	TMap<FElementTargetKey, FActiveGameplayEffectHandle> CharacterEffects;
	TMap<FElementTargetKey, FElementVisualShardKey> VisualShards;
	TMap<FElementTargetKey, FBurnPresentation> BurnPresentations;
	TArray<FPendingBuildingSnapshotBatch> PendingBuildingSnapshotBatches;
	int32 PendingBuildingSnapshotBatchCursor = 0;

	FDelegateHandle AuthorityStepHandle;
	FDelegateHandle PostActorTickHandle;
	FDelegateHandle BuildingChangesHandle;
	FDelegateHandle WorldObjectChangesHandle;
	FDelegateHandle CharacterChangesHandle;
	uint64 NextRuntimeSourceId = 1;
	uint64 NextRuntimeSourceToken = 1;
	bool bInitialized = false;
};

FElementFireDomain::FElementFireDomain(UElementGameplayWorldSubsystem& InOwner)
	: Data(MakeUnique<FElementFireDomainData>(*this, InOwner))
{
}

FElementFireDomain::~FElementFireDomain()
{
	if (Data) Data->Shutdown();
}

bool FElementFireDomain::Initialize()
{
	return Data && Data->Initialize();
}

void FElementFireDomain::Shutdown()
{
	if (Data) Data->Shutdown();
}

FElementRuntimeFireSourceHandle FElementFireDomain::CreateFireballSource(const FVector& WorldLocation)
{
	return Data ? Data->CreateFireball(WorldLocation) : FElementRuntimeFireSourceHandle();
}

bool FElementFireDomain::RemoveRuntimeFireSource(const FElementRuntimeFireSourceHandle Handle)
{
	return Data && Data->RemoveRuntimeSource(Handle);
}

bool FElementFireDomain::SetStickFireInteractionState(
	const FWorldEntityId WorldEntityId,
	const bool bActive)
{
	return Data && Data->SetStickInteraction(WorldEntityId, bActive);
}

#if WITH_DEV_AUTOMATION_TESTS
bool FElementFireDomain::IsStickFireInteractionActiveForTesting(
	const FWorldEntityId WorldEntityId) const
{
	return Data && Data->IsStickInteractionActive(WorldEntityId);
}

int32 FElementFireDomain::GetBuildingHostCountForTesting() const
{
	return Data ? Data->BuildingTargets.Num() : 0;
}
#endif

const FFireRuleSnapshot& FElementFireDomain::GetRules() const
{
	check(Data);
	return Data->Rules;
}

bool FElementFireDomain::CapturePersistentState(
	const TConstArrayView<FWorldEntityId> EntityIds,
	TArray<FElementFirePersistentRecord>& OutRecords,
	FString& OutError) const
{
	return Data && Data->CapturePersistentState(EntityIds, OutRecords, OutError);
}

bool FElementFireDomain::RestorePersistentState(
	const FWorldChunkCoord& HomeChunk,
	const TConstArrayView<FElementFirePersistentRecord> Records,
	FString& OutError)
{
	return Data && Data->RestorePersistentState(HomeChunk, Records, OutError);
}

bool FElementFireDomain::RemovePersistentState(
	const FWorldChunkCoord& HomeChunk,
	const TConstArrayView<FWorldEntityId> EntityIds,
	const EElementPersistentRemovalSemantic Semantic,
	FString& OutError)
{
	return Data && Data->RemovePersistentState(HomeChunk, EntityIds, Semantic, OutError);
}

bool FElementFireDomain::CanRuntimeEvictPersistentState(const FWorldEntityId EntityId) const
{
	return Data && Data->CanRuntimeEvictPersistentState(EntityId);
}
