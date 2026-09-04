#include "BuildingWorldSubsystem.h"

#include "Collision/BuildCollisionHost.h"
#include "Collision/BuildCollisionProcessor.h"
#include "Definition/BuildingDefinition.h"
#include "Definition/WorldObjectDefinition.h"
#include "ElementSandboxBuilding.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildWorldIdentityFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "Placement/BuildPlacementEvaluator.h"
#include "Placement/BuildPlacementSurfaceQuery.h"
#include "Placement/BuildPlacementTypes.h"
#include "Processing/BuildProcessorScheduler.h"
#include "PresentationWorldSubsystem.h"
#include "Rendering/BuildPresentationSettings.h"
#include "Rendering/BuildPresentationMeshPoolApplicator.h"
#include "Rendering/BuildRenderDirtySet.h"
#include "Rendering/BuildRenderProcessor.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Storage/BuildingPersistenceExtension.h"
#include "Subsystems/SubsystemCollection.h"
#include "WorldStorageSubsystem.h"
#include "WorldObjectWorldSubsystem.h"
#include "Engine/StaticMesh.h"
#include "BuildingWorldRuntime.h"

#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
constexpr uint32 BuildPayloadMagic = 0x32444c42; // BLD2
constexpr uint16 BuildPayloadVersion = 2;
constexpr int32 MaximumBuildSectionBytes = 16 * 1024 * 1024;

struct FDecodedBuildSection final
{
	FName Id = NAME_None;
	uint16 Version = 0;
	TArray<uint8> Payload;
};

struct FDecodedBuildPayload final
{
	EBuildSpatialMobility Mobility = EBuildSpatialMobility::Static;
	TArray<FDecodedBuildSection> Sections;
};

bool DecodeBuildPayload(const TArray<uint8>& Payload, FDecodedBuildPayload& OutPayload, FString& OutError)
{
	OutPayload = {};
	if (Payload.IsEmpty())
	{
		return true;
	}
	FMemoryReader Reader(const_cast<TArray<uint8>&>(Payload), true);
	uint32 Magic = 0;
	uint16 Version = 0;
	uint8 Mobility = 0;
	Reader << Magic << Version << Mobility;
	if (Magic != BuildPayloadMagic || Version != BuildPayloadVersion ||
		Mobility > static_cast<uint8>(EBuildSpatialMobility::Dynamic))
	{
		OutError = TEXT("Building Payload 格式或版本不匹配；请重新生成种子存档。");
		return false;
	}
	OutPayload.Mobility = static_cast<EBuildSpatialMobility>(Mobility);
	uint16 SectionCount = 0;
	Reader << SectionCount;
	TSet<FName> SeenSections;
	OutPayload.Sections.Reserve(SectionCount);
	for (uint16 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
	{
		FString SectionName;
		FDecodedBuildSection& Section = OutPayload.Sections.AddDefaulted_GetRef();
		int32 PayloadSize = 0;
		Reader << SectionName << Section.Version << PayloadSize;
		Section.Id = FName(*SectionName);
		if (Section.Id.IsNone() || Section.Version == 0 || PayloadSize <= 0 || PayloadSize > MaximumBuildSectionBytes ||
			SeenSections.Contains(Section.Id) || Reader.Tell() + PayloadSize > Reader.TotalSize())
		{
			OutError = TEXT("Building Payload 包含非法或重复 Section。");
			return false;
		}
		SeenSections.Add(Section.Id);
		Section.Payload.SetNumUninitialized(PayloadSize);
		Reader.Serialize(Section.Payload.GetData(), PayloadSize);
	}
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize())
	{
		OutError = TEXT("Building Payload 截断或存在尾随字节。");
		return false;
	}
	return true;
}

uint32 NextBuildRevision(const uint32 Current) { return Current == MAX_uint32 ? 1 : Current + 1; }

uint64 NextBuildTransformRevision(const uint64 Current) { return Current == MAX_uint64 ? 1 : Current + 1; }

const UBuildingDefinition* FindEntityDefinition(const FBuildEntityRegistry& Registry, const FBuildEntityHandle Entity)
{
	const FBuildDefinitionFragment* DefinitionFragment = Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	return DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
}

FBuildPresentationResidencyConfig MakePresentationResidencyConfig()
{
	const UBuildPresentationSettings* Settings = GetDefault<UBuildPresentationSettings>();
	FBuildPresentationResidencyConfig Config;
	Config.StaticCellSize = Settings->StaticCellSize;
	Config.MinimumLocalRadius = Settings->MinimumLocalRadius;
	Config.LocalResidentTargetMeshParts = Settings->LocalResidentTargetMeshParts;
	Config.StableResidentTargetMeshParts = Settings->StableResidentTargetMeshParts;
	Config.TransitionReserveMeshParts = Settings->TransitionReserveMeshParts;
	Config.ResidentHardWatermarkMeshParts = Settings->ResidentHardWatermarkMeshParts;
	Config.EmergencyOverflowMeshParts = Settings->EmergencyOverflowMeshParts;
	Config.ForwardCoverageAngleDegrees = Settings->ForwardCoverageAngleDegrees;
	Config.FOVSafetyAngleDegrees = Settings->FOVSafetyAngleDegrees;
	Config.MinimumRecenterAngleDegrees = Settings->MinimumRecenterAngleDegrees;
	Config.FarSettleSeconds = Settings->FarSettleSeconds;
	Config.RapidRotationThresholdDegreesPerSecond = Settings->RapidRotationThresholdDegreesPerSecond;
	Config.RotationReversalWindowSeconds = Settings->RotationReversalWindowSeconds;
	Config.PromotionStableSeconds = Settings->PromotionStableSeconds;
	Config.UnstablePromotionLockSeconds = Settings->UnstablePromotionLockSeconds;
	Config.InitialMeshPoolWorkBudgetParts = Settings->InitialMeshPoolWorkBudgetParts;
	Config.MinimumMeshPoolWorkBudgetParts = Settings->MinimumMeshPoolWorkBudgetParts;
	Config.MaximumMeshPoolWorkBudgetParts = Settings->MaximumMeshPoolWorkBudgetParts;
	Config.LocalTransitionPublishBudgetEntitiesPerCycle = Settings->LocalTransitionPublishBudgetEntitiesPerCycle;
	Config.NormalInstanceApplyTargetMilliseconds = Settings->NormalInstanceApplyTargetMilliseconds;
	Config.EmergencyInstanceApplyTargetMilliseconds = Settings->EmergencyInstanceApplyTargetMilliseconds;
	Config.EvictionGraceSeconds = Settings->EvictionGraceSeconds;
	Config.EvictionFrequencyHz = Settings->EvictionFrequencyHz;
	Config.HotPromotionRadius = Settings->HotPromotionRadius;
	Config.SourceMovementThreshold = Settings->SourceMovementThreshold;
	Config.GameplayChunkSize = Settings->GameplayChunkSize;
	Config.GameplayChunkPadding = Settings->GameplayChunkPadding;
	return Config;
}

FBuildRenderClusterConfig MakeRenderClusterConfig()
{
	FBuildRenderClusterConfig Config;
	Config.StaticCellSize = GetDefault<UBuildPresentationSettings>()->StaticCellSize;
	return Config;
}

} // namespace

FBuildingWorldRuntime::FBuildingWorldRuntime()
	: Presentation(MakePresentationResidencyConfig(), MakeRenderClusterConfig())
{
}

namespace
{
class FScopedBuildQuerySnapshotTransaction final
{
public:
	explicit FScopedBuildQuerySnapshotTransaction(FBuildQuerySnapshotStream& InStream)
			: Stream(InStream), bOwnsTransaction(!Stream.IsInTransaction()),
			  bValid(!bOwnsTransaction || Stream.BeginTransaction())
	{
	}

	~FScopedBuildQuerySnapshotTransaction()
	{
		if (bOwnsTransaction && !bFinished)
		{
				Stream.CancelTransaction();
		}
	}

	bool IsValid() const { return bValid; }

	bool Finish(const bool bSucceeded)
	{
		if (!bValid || bFinished)
		{
			return false;
		}
		bFinished = true;
		if (!bOwnsTransaction)
		{
			return bSucceeded;
		}
		if (!bSucceeded)
		{
				Stream.CancelTransaction();
			return false;
		}
			return Stream.CommitTransaction();
	}

private:
	FBuildQuerySnapshotStream& Stream;
	bool bOwnsTransaction = false;
	bool bValid = false;
	bool bFinished = false;
};

uint64 MakeBuildShapeTransformRevision(const uint64 EntityRevision, const uint64 PartRevision,
									   const bool bHasPartTransform)
{
	if (!bHasPartTransform)
	{
		return EntityRevision;
	}
	// 两个独立时钟都保留在快照中；压缩值只供快速“是否变化”比较。
	const uint64 Combined = HashCombineFast(GetTypeHash(EntityRevision), GetTypeHash(PartRevision));
	return Combined == 0 ? 1 : Combined;
}

bool TryCompileBuildPartShape(const FBuildEntityHandle Entity, const UBuildingDefinition& Definition,
								  const FBuildWorldIdentityFragment& Identity, const int32 PartId,
								  const FTransform& EntityWorldTransform, const FTransform& PartLocalTransform,
								  const uint64 EntityTransformRevision, const uint64 PartTransformRevision,
								  const bool bHasPartTransform, const uint32 StateRevision,
								  FBuildShapeInstanceSnapshot& OutShape)
{
	OutShape = {};
	if (!Identity.WorldEntityId.IsSet() || !Definition.MeshParts.IsValidIndex(PartId) ||
		EntityWorldTransform.ContainsNaN() || PartLocalTransform.ContainsNaN() || EntityTransformRevision == 0 ||
		PartTransformRevision == 0 || StateRevision == 0)
	{
		return false;
	}

	const FBuildMeshPartDefinition& Part = Definition.MeshParts[PartId];
	if (!Part.Mesh)
	{
		return false;
	}
	FBuildPartShapeDefinition ResolvedGeometry;
	if (!Part.Shape.TryResolve(Part.Mesh->GetBoundingBox(), ResolvedGeometry))
	{
		return false;
	}

	OutShape.ShapeRef.WorldEntityId = Identity.WorldEntityId;
	OutShape.ShapeRef.Entity = Entity;
	OutShape.ShapeRef.PartId = PartId;
	OutShape.ShapeRef.ShapeId = 0;
	OutShape.DefinitionId = Definition.DefinitionId;
	OutShape.SurfaceProfileId = Part.SurfaceProfileId;
	OutShape.TemplateRevision = ResolvedGeometry.TemplateRevision;
	OutShape.EntityTransformRevision = EntityTransformRevision;
	OutShape.PartTransformRevision = PartTransformRevision;
	OutShape.TransformRevision =
		MakeBuildShapeTransformRevision(EntityTransformRevision, PartTransformRevision, bHasPartTransform);
	OutShape.StateRevision = StateRevision;
	OutShape.LocalGeometry = ResolvedGeometry;
	OutShape.WorldTransform = PartLocalTransform * EntityWorldTransform;
	OutShape.WorldBounds = ResolvedGeometry.CalculateBroadphaseBounds(OutShape.WorldTransform);
	return OutShape.IsValid();
}

void CompileBuildEntityShapesFromResolvedFragments(
	const FBuildEntityHandle Entity, const FBuildTransformFragment& Transform,
	const UBuildingDefinition& Definition, const FBuildWorldIdentityFragment& Identity,
	const FBuildPartTransformFragment* PartTransforms, const bool bUseCommittedTransforms,
	const TConstArrayView<int32> RequestedPartIds, TArray<FBuildShapeInstanceSnapshot>& OutShapes)
{
	OutShapes.Reset();
	if (Identity.StateRevision == 0)
	{
		return;
	}

	const FTransform& EntityWorldTransform =
		bUseCommittedTransforms ? Transform.CommittedWorldTransform : Transform.WorldTransform;
	auto CompilePart = [&](const int32 PartId)
	{
		if (!Definition.MeshParts.IsValidIndex(PartId))
		{
			return;
		}
		const bool bHasCurrentPartTransforms =
			PartTransforms && PartTransforms->LocalTransforms.Num() == Definition.MeshParts.Num();
		const bool bHasCommittedPartTransforms =
			PartTransforms && PartTransforms->CommittedLocalTransforms.Num() == Definition.MeshParts.Num();
		const bool bHasPartRevisions = PartTransforms && PartTransforms->Revisions.Num() == Definition.MeshParts.Num();
		const FTransform& PartLocalTransform = bUseCommittedTransforms && bHasCommittedPartTransforms
											   ? PartTransforms->CommittedLocalTransforms[PartId]
											   : (!bUseCommittedTransforms && bHasCurrentPartTransforms
													  ? PartTransforms->LocalTransforms[PartId]
													  : Definition.MeshParts[PartId].LocalTransform);
		const uint64 PartRevision = bHasPartRevisions ? PartTransforms->Revisions[PartId] : 1;
		FBuildShapeInstanceSnapshot Shape;
		if (TryCompileBuildPartShape(Entity, Definition, Identity, PartId, EntityWorldTransform, PartLocalTransform,
									 Transform.Revision, PartRevision, bHasPartRevisions, Identity.StateRevision,
									 Shape))
		{
			OutShapes.Add(MoveTemp(Shape));
		}
	};

	if (RequestedPartIds.IsEmpty())
	{
		OutShapes.Reserve(Definition.MeshParts.Num());
		for (int32 PartId = 0; PartId < Definition.MeshParts.Num(); ++PartId)
		{
			CompilePart(PartId);
		}
	}
	else
	{
		OutShapes.Reserve(RequestedPartIds.Num());
		for (const int32 PartId : RequestedPartIds)
		{
			CompilePart(PartId);
		}
	}
}

void CompileBuildEntityShapes(const FBuildEntityRegistry& Registry, const FBuildEntityHandle Entity,
								  const bool bUseCommittedTransforms, const TConstArrayView<int32> RequestedPartIds,
								  TArray<FBuildShapeInstanceSnapshot>& OutShapes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Create_CompileShapes);
	const FBuildTransformFragment* Transform = Registry.FindFragment<FBuildTransformFragment>(Entity);
	const FBuildDefinitionFragment* DefinitionFragment = Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const FBuildWorldIdentityFragment* Identity = Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
	const FBuildPartTransformFragment* PartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
	if (!Transform || !Definition || !Identity)
	{
		OutShapes.Reset();
		return;
	}
	CompileBuildEntityShapesFromResolvedFragments(
		Entity, *Transform, *Definition, *Identity, PartTransforms, bUseCommittedTransforms, RequestedPartIds, OutShapes);
}

bool BuildShapeRefLess(const FBuildShapeRef& Left, const FBuildShapeRef& Right)
{
	if (Left.WorldEntityId != Right.WorldEntityId)
	{
		return Left.WorldEntityId < Right.WorldEntityId;
	}
	if (Left.Entity.GetRegistryId() != Right.Entity.GetRegistryId())
	{
		return Left.Entity.GetRegistryId() < Right.Entity.GetRegistryId();
	}
	if (Left.Entity.GetIndex() != Right.Entity.GetIndex())
	{
		return Left.Entity.GetIndex() < Right.Entity.GetIndex();
	}
	if (Left.Entity.GetGeneration() != Right.Entity.GetGeneration())
	{
		return Left.Entity.GetGeneration() < Right.Entity.GetGeneration();
	}
	if (Left.PartId != Right.PartId)
	{
		return Left.PartId < Right.PartId;
	}
	return Left.ShapeId < Right.ShapeId;
}

bool BuildQuerySnapshotChangeLess(const FBuildQuerySnapshotChange& Left,
								  const FBuildQuerySnapshotChange& Right)
{
	if (Left.WorldEntityId != Right.WorldEntityId) return Left.WorldEntityId < Right.WorldEntityId;
	if (Left.PartId != Right.PartId) return Left.PartId < Right.PartId;
	return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
}

void AppendBuildShapeRemovalChanges(
	const TConstArrayView<FBuildShapeInstanceSnapshot> Shapes,
	const EBuildQuerySnapshotChangeKind RemovalKind,
	const uint32 PublishedStateRevision,
	const int64 EffectiveTimeMilliseconds,
	TArray<FBuildQuerySnapshotChange>& OutChanges)
{
	OutChanges.Reserve(OutChanges.Num() + Shapes.Num());
	for (const FBuildShapeInstanceSnapshot& Shape : Shapes)
	{
		FBuildQuerySnapshotChange& Record = OutChanges.AddDefaulted_GetRef();
		Record.Kind = RemovalKind;
		Record.WorldEntityId = Shape.ShapeRef.WorldEntityId;
		Record.Entity = Shape.ShapeRef.Entity;
		Record.PartId = Shape.ShapeRef.PartId;
		Record.StateRevision = PublishedStateRevision;
		Record.EffectiveTimeMilliseconds = EffectiveTimeMilliseconds;
		Record.Previous = Shape;
	}
}

bool PublishBuildShapeTransition(FBuildQuerySnapshotStream& Stream,
									 const TConstArrayView<FBuildShapeInstanceSnapshot> OldShapes,
									 const TConstArrayView<FBuildShapeInstanceSnapshot> NewShapes,
									 const EBuildQuerySnapshotChangeKind RetainedKind,
									 const EBuildQuerySnapshotChangeKind RemovedKind,
									 const uint32 PublishedStateRevision, const int64 EffectiveTimeMilliseconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Create_PublishShapes);
	if (NewShapes.IsEmpty())
	{
		TArray<FBuildQuerySnapshotChange> Changes;
		AppendBuildShapeRemovalChanges(
			OldShapes, RemovedKind, PublishedStateRevision, EffectiveTimeMilliseconds, Changes);
		Changes.Sort(BuildQuerySnapshotChangeLess);
		return Changes.IsEmpty() || Stream.Publish(Changes);
	}
	if (OldShapes.IsEmpty())
	{
		TArray<FBuildQuerySnapshotChange> Changes;
		Changes.Reserve(NewShapes.Num());
		for (const FBuildShapeInstanceSnapshot& Shape : NewShapes)
		{
			FBuildQuerySnapshotChange& Record = Changes.AddDefaulted_GetRef();
			Record.Kind = EBuildQuerySnapshotChangeKind::Upsert;
			Record.WorldEntityId = Shape.ShapeRef.WorldEntityId;
			Record.Entity = Shape.ShapeRef.Entity;
			Record.PartId = Shape.ShapeRef.PartId;
			Record.StateRevision = PublishedStateRevision;
			Record.EffectiveTimeMilliseconds = EffectiveTimeMilliseconds;
			Record.Current = Shape;
		}
		Changes.Sort(BuildQuerySnapshotChangeLess);
		return Changes.IsEmpty() || Stream.Publish(Changes);
	}
	bool bAlignedShapeRefs = OldShapes.Num() == NewShapes.Num();
	for (int32 ShapeIndex = 0; bAlignedShapeRefs && ShapeIndex < OldShapes.Num(); ++ShapeIndex)
	{
		bAlignedShapeRefs = OldShapes[ShapeIndex].ShapeRef == NewShapes[ShapeIndex].ShapeRef;
	}
	if (bAlignedShapeRefs)
	{
		TArray<FBuildQuerySnapshotChange> Changes;
		Changes.Reserve(NewShapes.Num());
		for (int32 ShapeIndex = 0; ShapeIndex < NewShapes.Num(); ++ShapeIndex)
		{
			const FBuildShapeInstanceSnapshot& Shape = NewShapes[ShapeIndex];
			FBuildQuerySnapshotChange& Record = Changes.AddDefaulted_GetRef();
			Record.Kind = RetainedKind;
			Record.WorldEntityId = Shape.ShapeRef.WorldEntityId;
			Record.Entity = Shape.ShapeRef.Entity;
			Record.PartId = Shape.ShapeRef.PartId;
			Record.StateRevision = PublishedStateRevision;
			Record.EffectiveTimeMilliseconds = EffectiveTimeMilliseconds;
			Record.Previous = OldShapes[ShapeIndex];
			Record.Current = Shape;
		}
		Changes.Sort(BuildQuerySnapshotChangeLess);
		return Changes.IsEmpty() || Stream.Publish(Changes);
	}

	TMap<FBuildShapeRef, const FBuildShapeInstanceSnapshot*> OldByRef;
	TMap<FBuildShapeRef, const FBuildShapeInstanceSnapshot*> NewByRef;
	TArray<FBuildShapeRef> Refs;
	for (const FBuildShapeInstanceSnapshot& Shape : OldShapes)
	{
		OldByRef.Add(Shape.ShapeRef, &Shape);
		Refs.AddUnique(Shape.ShapeRef);
	}
	for (const FBuildShapeInstanceSnapshot& Shape : NewShapes)
	{
		NewByRef.Add(Shape.ShapeRef, &Shape);
		Refs.AddUnique(Shape.ShapeRef);
	}
	Refs.Sort(BuildShapeRefLess);

	TArray<FBuildQuerySnapshotChange> Changes;
	Changes.Reserve(Refs.Num());
	for (const FBuildShapeRef& Ref : Refs)
	{
		const FBuildShapeInstanceSnapshot* const* OldPtr = OldByRef.Find(Ref);
		const FBuildShapeInstanceSnapshot* const* NewPtr = NewByRef.Find(Ref);
		const FBuildShapeInstanceSnapshot* OldShape = OldPtr ? *OldPtr : nullptr;
		const FBuildShapeInstanceSnapshot* NewShape = NewPtr ? *NewPtr : nullptr;
		if (!OldShape && !NewShape) continue;
		FBuildQuerySnapshotChange& Record = Changes.AddDefaulted_GetRef();
		Record.Kind = OldShape && !NewShape
			? RemovedKind : (!OldShape && NewShape ? EBuildQuerySnapshotChangeKind::Upsert : RetainedKind);
		const FBuildShapeInstanceSnapshot& IdentityShape = NewShape ? *NewShape : *OldShape;
		Record.WorldEntityId = IdentityShape.ShapeRef.WorldEntityId;
		Record.Entity = IdentityShape.ShapeRef.Entity;
		Record.PartId = IdentityShape.ShapeRef.PartId;
		Record.StateRevision = PublishedStateRevision;
		Record.EffectiveTimeMilliseconds = EffectiveTimeMilliseconds;
		if (OldShape) Record.Previous = *OldShape;
		if (NewShape) Record.Current = *NewShape;
	}
	Changes.Sort(BuildQuerySnapshotChangeLess);
	return Changes.IsEmpty() || Stream.Publish(Changes);
}

int64 GetBuildHostEffectiveTimeMilliseconds(const FBuildingWorldRuntime& Runtime, const UWorld& World)
{
	if (const UWorldStorageSubsystem* Storage = Runtime.Persistence.WorldStorage.Get())
	{
		return Storage->GetWorldSimulationTimeMilliseconds();
	}
	return FMath::Max<int64>(0, FMath::RoundToInt64(World.GetTimeSeconds() * 1000.0));
}
} // namespace

class FBuildingWorldStorageAdapter final : public IWorldStorageDomainAdapter
{
public:
	explicit FBuildingWorldStorageAdapter(UBuildingWorldSubsystem& InOwner) : Owner(&InOwner) {}

	virtual EWorldEntityDomain GetDomain() const override { return EWorldEntityDomain::Building; }

	virtual EWorldStorageRestorePhase GetRestorePhase() const override { return EWorldStorageRestorePhase::Primary; }

	virtual bool CaptureBatch(const TConstArrayView<FWorldEntityId> EntityIds,
							  TArray<FWorldPersistentEntityRecord>& OutRecords, FString& OutError) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_CaptureBatch);
		UBuildingWorldSubsystem* Building = Owner.Get();
		if (!Building || !Building->Runtime)
		{
			OutError = TEXT("Building Runtime 不可用。");
			return false;
		}
		OutRecords.Reserve(OutRecords.Num() + EntityIds.Num());
		for (const FWorldEntityId EntityId : EntityIds)
		{
			const FBuildEntityHandle Entity = Building->FindEntity(EntityId);
			const FBuildWorldIdentityFragment* Identity =
				Building->Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
			const FBuildDefinitionFragment* DefinitionFragment =
				Building->Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
			const FBuildTransformFragment* Transform =
				Building->Runtime->Core.Registry.FindFragment<FBuildTransformFragment>(Entity);
			const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			EBuildSpatialMobility Mobility = EBuildSpatialMobility::Static;
			if (!Entity.IsSet() || !Identity || Identity->WorldEntityId != EntityId || Identity->StateRevision == 0 ||
				!Definition || !Transform || !Building->Runtime->Core.SpatialIndex.TryGetMobility(Entity, Mobility))
			{
				OutError = FString::Printf(TEXT("Building %llu 缺少可持久化核心状态。"), EntityId.GetValue());
				return false;
			}

			TArray<FDecodedBuildSection> Sections;
			for (const FName SectionId : Building->Runtime->Persistence.ExtensionOrder)
			{
				const TSharedRef<IBuildingPersistenceExtension>& Extension =
					Building->Runtime->Persistence.Extensions.FindChecked(SectionId);
				FDecodedBuildSection Section;
				Section.Id = SectionId;
				Section.Version = Extension->GetSectionVersion();
				if (Section.Version == 0 ||
					!Extension->Capture(Building->Runtime->Core.Registry, Entity, Section.Payload, OutError))
				{
					return false;
				}
				if (!Section.Payload.IsEmpty())
				{
					Sections.Add(MoveTemp(Section));
				}
			}

			FWorldPersistentEntityRecord& Record = OutRecords.AddDefaulted_GetRef();
			Record.EntityId = EntityId;
			Record.Domain = EWorldEntityDomain::Building;
			Record.DefinitionId = Definition->DefinitionId;
			Record.WorldTransform = Transform->WorldTransform;
			Record.StateRevision = Identity->StateRevision;
			if (Mobility == EBuildSpatialMobility::Static && Sections.IsEmpty())
			{
				continue;
			}

			FMemoryWriter Writer(Record.Payload, true);
			uint32 Magic = BuildPayloadMagic;
			uint16 Version = BuildPayloadVersion;
			uint8 MobilityByte = static_cast<uint8>(Mobility);
			Writer << Magic << Version << MobilityByte;
			uint16 SectionCount = static_cast<uint16>(Sections.Num());
			Writer << SectionCount;
			for (FDecodedBuildSection& Section : Sections)
			{
				FString SectionName = Section.Id.ToString();
				int32 PayloadSize = Section.Payload.Num();
				Writer << SectionName << Section.Version << PayloadSize;
				Writer.Serialize(Section.Payload.GetData(), PayloadSize);
			}
			if (Writer.IsError())
			{
				OutError = TEXT("Building Payload 编码失败。");
				return false;
			}
		}
		return true;
	}

	virtual bool RestoreBatch(const FWorldChunkCoord& HomeChunk,
							  const TConstArrayView<FWorldPersistentEntityRecord> Records, FString& OutError) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_RestoreBatch);
		UBuildingWorldSubsystem* Building = Owner.Get();
		if (!Building || !Building->Runtime)
		{
			OutError = TEXT("Building Runtime 不可用。");
			return false;
		}
		FScopedBuildQuerySnapshotTransaction JournalTransaction(Building->Runtime->Core.QuerySnapshots);
		if (!JournalTransaction.IsValid())
		{
			OutError = TEXT("Building 查询快照流无法开始 Restore 事务。");
			return false;
		}
		struct FAppliedRecord final
		{
			FWorldEntityId EntityId;
			bool bCreated = false;
			TOptional<FWorldPersistentEntityRecord> Backup;
		};
		TArray<FAppliedRecord, TInlineAllocator<8>> Applied;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_ApplyRecords);
			for (const FWorldPersistentEntityRecord& Record : Records)
			{
				if (!Record.IsValid() || Record.Domain != EWorldEntityDomain::Building ||
					FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != HomeChunk)
				{
					OutError = TEXT("Building Restore 收到非法记录或错误 HomeChunk。");
					Rollback(*Building, Applied);
					return false;
				}
				FAppliedRecord AppliedRecord;
				AppliedRecord.EntityId = Record.EntityId;
				AppliedRecord.bCreated = !Building->FindEntity(Record.EntityId).IsSet();
				if (!AppliedRecord.bCreated)
				{
					TArray<FWorldPersistentEntityRecord> BackupRecords;
					if (!CaptureBatch(MakeArrayView(&Record.EntityId, 1), BackupRecords, OutError) ||
						BackupRecords.Num() != 1)
					{
						Rollback(*Building, Applied);
						return false;
					}
					AppliedRecord.Backup = MoveTemp(BackupRecords[0]);
				}
				Applied.Add(MoveTemp(AppliedRecord));
				if (!ApplyRecord(*Building, Record, OutError))
				{
					Rollback(*Building, Applied);
					return false;
				}
			}
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_CommitQuerySnapshots);
			return JournalTransaction.Finish(true);
		}
	}

	virtual bool RuntimeEvictBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
								   FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UBuildingWorldSubsystem::ERemovalSemantic::RuntimeEvict, OutError);
	}

	virtual bool GameplayDestroyBatch(const FWorldChunkCoord& HomeChunk,
									  const TConstArrayView<FWorldEntityId> EntityIds, FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UBuildingWorldSubsystem::ERemovalSemantic::GameplayDestroy, OutError);
	}

	virtual bool LeaveInterestBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
									FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UBuildingWorldSubsystem::ERemovalSemantic::LeaveInterest, OutError);
	}

	virtual bool RollbackRestoreBatch(const FWorldChunkCoord& HomeChunk,
									  const TConstArrayView<FWorldEntityId> EntityIds, FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UBuildingWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback,
						   OutError);
	}

	virtual bool CanRuntimeEvict(const FWorldEntityId EntityId) const override
	{
		const UBuildingWorldSubsystem* Building = Owner.Get();
		return Building && Building->IsEntityAlive(Building->FindEntity(EntityId));
	}

	bool RemoveBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
					 const UBuildingWorldSubsystem::ERemovalSemantic Semantic, FString& OutError)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_RemoveBatch);
		UBuildingWorldSubsystem* Building = Owner.Get();
		if (!Building || !Building->Runtime)
		{
			OutError = TEXT("Building Runtime 不可用。");
			return false;
		}
		FScopedBuildQuerySnapshotTransaction JournalTransaction(Building->Runtime->Core.QuerySnapshots);
		if (!JournalTransaction.IsValid())
		{
			OutError = TEXT("Building 查询快照流无法开始移除事务。");
			return false;
		}
		const bool bGameplayDestroy = Semantic == UBuildingWorldSubsystem::ERemovalSemantic::GameplayDestroy;
		const bool bRequiresRollbackCapture = Building->EntityLocalRemovedEvent.IsBound()
			|| (bGameplayDestroy && (Building->EntityPreDestroyEvent.IsBound()
				|| Building->EntityDestroyedEvent.IsBound()
				|| Building->GetWorldRef().GetNetMode() != NM_Client));
		if (bRequiresRollbackCapture)
		{
			TArray<FWorldPersistentEntityRecord> Backups;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_CaptureRemovalBackups);
				if (!CaptureBatch(EntityIds, Backups, OutError) || Backups.Num() != EntityIds.Num())
				{
					return false;
				}
				for (const FWorldPersistentEntityRecord& Backup : Backups)
				{
					if (FWorldChunkCoord::FromWorldLocation(Backup.WorldTransform.GetLocation()) != HomeChunk)
					{
						OutError = TEXT("Building 批量移除的 HomeChunk 不匹配。");
						return false;
					}
				}
			}
			int32 DestroyedCount = 0;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_DestroyRemovalEntities);
				for (const FWorldEntityId EntityId : EntityIds)
				{
					if (!Building->DestroyEntityInternal(Building->FindEntity(EntityId), Semantic))
					{
						for (int32 Index = 0; Index < DestroyedCount; ++Index)
						{
							FString Ignored;
							ApplyRecord(*Building, Backups[Index], Ignored);
						}
						OutError = FString::Printf(TEXT("Building %llu 批量移除失败。"), EntityId.GetValue());
						return false;
					}
					++DestroyedCount;
				}
			}
			return JournalTransaction.Finish(true);
		}

		struct FPreparedRemoval final
		{
			FBuildEntityHandle Entity;
			FWorldEntityId WorldEntityId;
		};
		TArray<FPreparedRemoval> Prepared;
		Prepared.Reserve(EntityIds.Num());
		TArray<FBuildQuerySnapshotChange> RemovalChanges;
		RemovalChanges.Reserve(EntityIds.Num());
		TArray<FBuildShapeInstanceSnapshot> RemovedShapes;
		TSet<FWorldEntityId> Seen;
		Seen.Reserve(EntityIds.Num());
		TArray<TSharedRef<IBuildingPersistenceExtension>, TInlineAllocator<4>> RemovalValidators;
		RemovalValidators.Reserve(Building->Runtime->Persistence.ExtensionOrder.Num());
		for (const FName SectionId : Building->Runtime->Persistence.ExtensionOrder)
		{
			const TSharedRef<IBuildingPersistenceExtension>& Extension =
				Building->Runtime->Persistence.Extensions.FindChecked(SectionId);
			if (Extension->GetSectionVersion() == 0)
			{
				OutError = TEXT("Building 批量移除发现无效的持久化扩展版本。");
				return false;
			}
			RemovalValidators.Add(Extension);
		}

		EBuildQuerySnapshotChangeKind RemovalKind = EBuildQuerySnapshotChangeKind::RuntimeEvict;
		switch (Semantic)
		{
		case UBuildingWorldSubsystem::ERemovalSemantic::GameplayDestroy:
			RemovalKind = EBuildQuerySnapshotChangeKind::GameplayDestroy;
			break;
		case UBuildingWorldSubsystem::ERemovalSemantic::RuntimeEvict:
			RemovalKind = EBuildQuerySnapshotChangeKind::RuntimeEvict;
			break;
		case UBuildingWorldSubsystem::ERemovalSemantic::LeaveInterest:
			RemovalKind = EBuildQuerySnapshotChangeKind::LeaveInterest;
			break;
		case UBuildingWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback:
			RemovalKind = EBuildQuerySnapshotChangeKind::FailedRegistrationRollback;
			break;
		default:
			checkNoEntry();
			return false;
		}

		const int64 EffectiveTimeMilliseconds =
			GetBuildHostEffectiveTimeMilliseconds(*Building->Runtime, Building->GetWorldRef());
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_PrevalidateRemoval);
			for (const FWorldEntityId EntityId : EntityIds)
			{
				bool bAlreadySeen = false;
				Seen.Add(EntityId, &bAlreadySeen);
				if (!EntityId.IsSet() || bAlreadySeen)
				{
					OutError = TEXT("Building 批量移除包含无效或重复 EntityId。");
					return false;
				}
				const FBuildEntityHandle Entity = Building->FindEntity(EntityId);
				const FBuildWorldIdentityFragment* Identity =
					Building->Runtime->Core.Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
				const FBuildDefinitionFragment* DefinitionFragment =
					Building->Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
				const FBuildTransformFragment* Transform =
					Building->Runtime->Core.Registry.FindFragment<FBuildTransformFragment>(Entity);
				const UBuildingDefinition* Definition =
					DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
				EBuildSpatialMobility Mobility = EBuildSpatialMobility::Static;
				if (!Entity.IsSet() || !Identity || Identity->WorldEntityId != EntityId
					|| Identity->StateRevision == 0 || !Definition || !Transform
					|| FWorldChunkCoord::FromWorldLocation(Transform->WorldTransform.GetLocation()) != HomeChunk
					|| !Building->Runtime->Core.SpatialIndex.TryGetMobility(Entity, Mobility))
				{
					OutError = FString::Printf(
						TEXT("Building %llu 批量移除预检失败或 HomeChunk 不匹配。"), EntityId.GetValue());
					return false;
				}
				for (const TSharedRef<IBuildingPersistenceExtension>& Extension : RemovalValidators)
				{
					if (!Extension->ValidateRemovalState(
							Building->Runtime->Core.Registry, Entity, OutError))
					{
						return false;
					}
				}

				FPreparedRemoval& Entry = Prepared.AddDefaulted_GetRef();
				Entry.Entity = Entity;
				Entry.WorldEntityId = EntityId;
				const uint32 RemovedRevision = bGameplayDestroy
					? NextBuildRevision(Identity->StateRevision) : Identity->StateRevision;
				const FBuildPartTransformFragment* PartTransforms =
					Building->Runtime->Core.Registry.FindFragment<FBuildPartTransformFragment>(Entity);
				CompileBuildEntityShapesFromResolvedFragments(
					Entity, *Transform, *Definition, *Identity, PartTransforms, true, {}, RemovedShapes);
				AppendBuildShapeRemovalChanges(
					RemovedShapes, RemovalKind, RemovedRevision, EffectiveTimeMilliseconds, RemovalChanges);
			}
		}

		RemovalChanges.Sort(BuildQuerySnapshotChangeLess);
		if ((!RemovalChanges.IsEmpty() && !Building->Runtime->Core.QuerySnapshots.Publish(RemovalChanges))
			|| (!Building->GetWorldRef().IsNetMode(NM_DedicatedServer)
				&& !Building->RequestPresentationProjection()))
		{
			OutError = TEXT("Building 批量移除无法预提交 Query Snapshot 或表现投影请求。");
			return false;
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_CommitPrevalidatedRemoval);
			for (const FPreparedRemoval& Entry : Prepared)
			{
				if (!Building->CommitPrevalidatedEntityRemoval(
						Entry.Entity, Entry.WorldEntityId, Semantic, true))
				{
					OutError = FString::Printf(
						TEXT("Building %llu 在全批预检后提交移除失败。"), Entry.WorldEntityId.GetValue());
					return false;
				}
			}
		}
		return JournalTransaction.Finish(true);
	}

private:
	static bool ApplyRecord(UBuildingWorldSubsystem& Building, const FWorldPersistentEntityRecord& Record,
							FString& OutError)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_ApplyRecord);
		FDecodedBuildPayload Payload;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Building_WorldStorage_DecodePayload);
			if (!DecodeBuildPayload(Record.Payload, Payload, OutError))
			{
				return false;
			}
		}
		TMap<FName, const FDecodedBuildSection*> Sections;
		for (const FDecodedBuildSection& Section : Payload.Sections)
		{
			const TSharedRef<IBuildingPersistenceExtension>* Extension =
				Building.Runtime->Persistence.Extensions.Find(Section.Id);
			if (!Extension || Extension->Get().GetSectionVersion() != Section.Version)
			{
				OutError = FString::Printf(TEXT("Building Section %s 缺失或版本不匹配；请重新生成种子存档。"),
										   *Section.Id.ToString());
				return false;
			}
			Sections.Add(Section.Id, &Section);
		}

		UBuildingDefinition* Definition = Building.FindDefinition(Record.DefinitionId);
		if (!Definition)
		{
			OutError = FString::Printf(TEXT("Building Definition 未注册：%s。"), *Record.DefinitionId.ToString());
			return false;
		}
		FBuildEntityHandle Entity = Building.FindEntity(Record.EntityId);
		const bool bCreated = !Entity.IsSet();
		TArray<float, TInlineAllocator<4>> PreviousRenderCustomData;
		if (!bCreated)
		{
			if (const FBuildRenderCustomDataFragment* Custom =
					Building.Runtime->Core.Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity))
			{
				PreviousRenderCustomData = Custom->Values;
			}
		}
		if (bCreated)
		{
			Entity = Building.CreateEntityInternal(*Definition, Record.WorldTransform, Payload.Mobility,
												   Record.EntityId, Record.StateRevision, false);
			if (!Entity.IsSet())
			{
				OutError = TEXT("Building ECS Restore 创建失败。");
				return false;
			}
		}
		else
		{
			const FBuildDefinitionFragment* ExistingDefinition =
				Building.Runtime->Core.Registry.FindFragment<FBuildDefinitionFragment>(Entity);
			FBuildTransformFragment* Transform =
				Building.Runtime->Core.Registry.FindMutableFragment<FBuildTransformFragment>(Entity);
			FBuildWorldIdentityFragment* Identity =
				Building.Runtime->Core.Registry.FindMutableFragment<FBuildWorldIdentityFragment>(Entity);
			if (!ExistingDefinition || ExistingDefinition->Definition.Get() != Definition || !Transform || !Identity)
			{
				OutError = TEXT("同 ID Building 的 Definition 冲突或核心 Fragment 缺失。");
				return false;
			}
			Transform->WorldTransform = Record.WorldTransform;
			if (!Building.Runtime->Core.SpatialIndex.SetMobility(Entity, Payload.Mobility) ||
				!Building.CommitEntityTransformChangeInternal(Entity, false))
			{
				OutError = TEXT("Building ECS Restore 更新 Transform/Mobility 失败。");
				return false;
			}
			Identity->StateRevision = Record.StateRevision;
		}

		// Definition 已为新实体写入规范默认 Fragment。缺失的可选 Section
		// 不应逐扩展重放默认值；已有实体仍需用空 Section 清除旧状态。
		const TConstArrayView<FName> ExtensionIds =
			bCreated ? TConstArrayView<FName>() : TConstArrayView<FName>(Building.Runtime->Persistence.ExtensionOrder);
		for (const FName ExtensionId : ExtensionIds)
		{
			const TSharedRef<IBuildingPersistenceExtension>& Extension =
				Building.Runtime->Persistence.Extensions.FindChecked(ExtensionId);
			const FDecodedBuildSection* const* SectionPtr = Sections.Find(ExtensionId);
			const FDecodedBuildSection* Section = SectionPtr ? *SectionPtr : nullptr;
			if (!Extension->Restore(Building.Runtime->Core.Registry, Entity,
									Section ? Section->Version : Extension->GetSectionVersion(),
									Section ? TConstArrayView<uint8>(Section->Payload) : TConstArrayView<uint8>(),
									OutError))
			{
				if (bCreated)
				{
					Building.DestroyEntityInternal(
						Entity, UBuildingWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback);
				}
				return false;
			}
		}
		if (bCreated)
		{
			for (const FDecodedBuildSection& Section : Payload.Sections)
			{
				const TSharedRef<IBuildingPersistenceExtension>& Extension =
					Building.Runtime->Persistence.Extensions.FindChecked(Section.Id);
				if (!Extension->Restore(Building.Runtime->Core.Registry, Entity, Section.Version, Section.Payload,
										OutError))
				{
					Building.DestroyEntityInternal(
						Entity, UBuildingWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback);
					return false;
				}
			}
		}
		if (!bCreated)
		{
			const FBuildRenderCustomDataFragment* Custom =
				Building.Runtime->Core.Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity);
			if (Custom && Custom->Values != PreviousRenderCustomData && !Building.CommitRenderCustomDataChange(Entity))
			{
				OutError = TEXT("Building Restore 无法提交 Derived Render Custom Data。");
				return false;
			}
		}
		// 新建且没有扩展状态时，CreateEntityInternal 已用最终 Revision 发布完整 Upsert；
		// 再发布同形 Metadata 只会重复编译 Shape 并放大所有下游投影队列。
		if (!bCreated || !Payload.Sections.IsEmpty())
		{
			TArray<FBuildShapeInstanceSnapshot> Shapes;
			CompileBuildEntityShapes(Building.Runtime->Core.Registry, Entity, true, {}, Shapes);
			if (!PublishBuildShapeTransition(
					Building.Runtime->Core.QuerySnapshots, Shapes, Shapes, EBuildQuerySnapshotChangeKind::Metadata,
					EBuildQuerySnapshotChangeKind::ShapeRemove, Record.StateRevision,
					GetBuildHostEffectiveTimeMilliseconds(*Building.Runtime, Building.GetWorldRef())))
			{
				OutError = TEXT("Building Restore 无法发布 Query Snapshot Metadata。");
				return false;
			}
		}
		return true;
	}

	template <typename TAppliedRecord, typename AllocatorType>
	static void Rollback(UBuildingWorldSubsystem& Building, const TArray<TAppliedRecord, AllocatorType>& Applied)
	{
		for (int32 Index = Applied.Num() - 1; Index >= 0; --Index)
		{
			const TAppliedRecord& Entry = Applied[Index];
			if (Entry.bCreated)
			{
				Building.DestroyEntityInternal(Building.FindEntity(Entry.EntityId),
											   UBuildingWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback);
			}
			else if (Entry.Backup.IsSet())
			{
				FString Ignored;
				ApplyRecord(Building, Entry.Backup.GetValue(), Ignored);
			}
		}
	}

	TWeakObjectPtr<UBuildingWorldSubsystem> Owner;
};

#include "BuildingWorldLifecycle.inl"
#include "BuildingWorldEntities.inl"
#include "BuildingWorldPresentation.inl"
