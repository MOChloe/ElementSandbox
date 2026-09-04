#include "Collision/WorldObjectCollisionWorldSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Definition/WorldObjectDefinition.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Subsystems/SubsystemCollection.h"
#include "WorldObjectWorldSubsystem.h"

CSV_DEFINE_CATEGORY(WorldObjectCollision, true);

namespace
{
	constexpr float EngineCubeHalfExtent = 50.0f;

	enum class EWorldObjectCollisionAddPriority : uint8
	{
		None,
		Regular,
		Immediate
	};

	struct FWorldObjectCollisionGraceDeadline final
	{
		double DeadlineSeconds = 0.0;
		FWorldObjectEntityHandle Entity;
		uint32 Generation = 0;
	};

	struct FWorldObjectCollisionDeadlineMinHeap final
	{
		bool operator()(const FWorldObjectCollisionGraceDeadline& Left,
			const FWorldObjectCollisionGraceDeadline& Right) const
		{
			// UE 的 HeapPush/HeapPop 使用最小堆语义，较早到期项必须排在堆顶。
			return Left.DeadlineSeconds < Right.DeadlineSeconds;
		}
	};

	bool AreSourcesEquivalentIgnoringRevision(
		const FWorldObjectCollisionSource& Left,
		const FWorldObjectCollisionSource& Right)
	{
		const auto BoxesEqual = [](const FBox& A, const FBox& B)
		{
			return A.IsValid == B.IsValid
				&& (A.IsValid == 0 || (A.Min.Equals(B.Min, 0.01) && A.Max.Equals(B.Max, 0.01)));
		};
		return Left.SubjectLocation.Equals(Right.SubjectLocation, 0.01)
			&& Left.ViewLocation.Equals(Right.ViewLocation, 0.01)
			&& Left.ViewDirection.Equals(Right.ViewDirection, 0.0001)
			&& Left.Velocity.Equals(Right.Velocity, 0.01)
			&& BoxesEqual(Left.PawnContactBounds, Right.PawnContactBounds)
			&& BoxesEqual(Left.ImmediateBounds, Right.ImmediateBounds)
			&& BoxesEqual(Left.PrefetchBounds, Right.PrefetchBounds)
			&& BoxesEqual(Left.RetentionBounds, Right.RetentionBounds);
	}

	bool TryBuildCollisionProjection(
		const UWorldObjectWorldSubsystem& WorldObjects,
		const FWorldObjectEntityHandle Entity,
		FTransform& OutInstanceTransform,
		FBox& OutWorldBounds,
		EWorldObjectPhysicsCollisionPolicy& OutPolicy)
	{
		const FWorldObjectEntityRegistry& Registry = WorldObjects.GetRegistry();
		const FWorldObjectMotionFragment* Motion =
			Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
		const FWorldObjectTransformFragment* Transform =
			Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
		const FWorldObjectPhysicsBodyFragment* Physics =
			Registry.FindFragment<FWorldObjectPhysicsBodyFragment>(Entity);
		const UWorldObjectProxyComponent* Proxy = WorldObjects.GetProxy(Entity);
		const AWorldObjectPhysicsProxyActor* PhysicsProxy = Proxy
			? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
		const bool bWaitingForClientPhysics = Motion && Motion->State == EWorldObjectMotionState::Physics
			&& WorldObjects.GetWorld()->GetNetMode() == NM_Client
			&& (!Proxy || (PhysicsProxy && !PhysicsProxy->HasClientPhysicsProjection()));
		if (!Motion || (Motion->State != EWorldObjectMotionState::Dormant && !bWaitingForClientPhysics) || !Transform || !Physics
			|| Physics->LocalCollisionCenter.ContainsNaN()
			|| Physics->LocalCollisionExtent.ContainsNaN()
			|| Physics->LocalCollisionExtent.GetMin() <= UE_SMALL_NUMBER)
		{
			return false;
		}

		OutPolicy = Physics->CollisionPolicy;
		const FVector LocalExtent = Physics->LocalCollisionExtent;
		const FBox LocalBounds(
			Physics->LocalCollisionCenter - LocalExtent,
			Physics->LocalCollisionCenter + LocalExtent);
		OutWorldBounds = LocalBounds.TransformBy(Transform->WorldTransform);
		OutInstanceTransform = FTransform(
			FQuat::Identity,
			Physics->LocalCollisionCenter,
			LocalExtent / EngineCubeHalfExtent) * Transform->WorldTransform;
		return OutWorldBounds.IsValid != 0 && !OutWorldBounds.ContainsNaN()
			&& !OutInstanceTransform.ContainsNaN();
	}
}

class FWorldObjectCollisionData final
{
public:
	struct FSourceSlot final
	{
		FWorldObjectCollisionSource Source;
		TSet<FWorldObjectEntityHandle> Immediate;
		TSet<FWorldObjectEntityHandle> Prefetch;
		TSet<FWorldObjectEntityHandle> Retention;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAlive = false;
		bool bDirty = false;
		/** 仅 Source 自身移动/速度变化时允许接触走廊预唤醒；生命周期刷新不得重复触发。 */
		bool bContactSweepDirty = false;
	};

	struct FCollisionSlot final
	{
		int32 InstanceIndex = INDEX_NONE;
		int32 ImmediateReferences = 0;
		int32 PrefetchReferences = 0;
		int32 RetentionReferences = 0;
		uint32 GraceGeneration = 0;
		EWorldObjectPhysicsCollisionPolicy Policy = EWorldObjectPhysicsCollisionPolicy::Standard;
		EWorldObjectCollisionAddPriority QueuedAddPriority = EWorldObjectCollisionAddPriority::None;
		bool bActive = false;
		bool bPendingRemove = false;
		bool bInGrace = false;
	};

	FCollisionSlot& EnsureSlot(const FWorldObjectEntityHandle Entity)
	{
		return CollisionSlots.FindOrAdd(Entity);
	}

	void ClearPendingAdd(FCollisionSlot& Slot)
	{
		if (Slot.QueuedAddPriority != EWorldObjectCollisionAddPriority::None)
		{
			Slot.QueuedAddPriority = EWorldObjectCollisionAddPriority::None;
			PendingAddCount = FMath::Max(0, PendingAddCount - 1);
		}
	}

	void QueueAdd(const FWorldObjectEntityHandle Entity, FCollisionSlot& Slot,
		const EWorldObjectCollisionAddPriority Priority)
	{
		if (Slot.bActive || Priority == EWorldObjectCollisionAddPriority::None) return;
		if (Slot.QueuedAddPriority == EWorldObjectCollisionAddPriority::None)
		{
			Slot.QueuedAddPriority = Priority;
			++PendingAddCount;
			(Priority == EWorldObjectCollisionAddPriority::Immediate
				? PendingImmediateAdds : PendingAdds).Add(Entity);
		}
		else if (Slot.QueuedAddPriority == EWorldObjectCollisionAddPriority::Regular
			&& Priority == EWorldObjectCollisionAddPriority::Immediate)
		{
			Slot.QueuedAddPriority = EWorldObjectCollisionAddPriority::Immediate;
			PendingImmediateAdds.Add(Entity);
		}
	}

	void CancelGrace(FCollisionSlot& Slot)
	{
		if (Slot.bInGrace)
		{
			Slot.bInGrace = false;
			++Slot.GraceGeneration;
		}
	}

	void CancelRemove(FCollisionSlot& Slot)
	{
		if (Slot.bPendingRemove)
		{
			Slot.bPendingRemove = false;
			PendingRemoveCount = FMath::Max(0, PendingRemoveCount - 1);
		}
	}

	void QueueRemove(const FWorldObjectEntityHandle Entity, FCollisionSlot& Slot)
	{
		if (!Slot.bActive || Slot.bPendingRemove) return;
		Slot.bPendingRemove = true;
		++PendingRemoveCount;
		PendingRemoves.Add(Entity);
	}

	void DiscardIfUnused(const FWorldObjectEntityHandle Entity)
	{
		FCollisionSlot* Slot = CollisionSlots.Find(Entity);
		if (Slot && !Slot->bActive && Slot->ImmediateReferences == 0
			&& Slot->PrefetchReferences == 0 && Slot->RetentionReferences == 0
			&& Slot->QueuedAddPriority == EWorldObjectCollisionAddPriority::None
			&& !Slot->bPendingRemove && !Slot->bInGrace)
		{
			CollisionSlots.Remove(Entity);
		}
	}

	void ReconcileSlot(const FWorldObjectEntityHandle Entity, const double NowSeconds,
		const FWorldObjectCollisionActivationConfig& InConfig)
	{
		FCollisionSlot* Slot = CollisionSlots.Find(Entity);
		if (!Slot) return;
		if (Slot->RetentionReferences > 0)
		{
			CancelGrace(*Slot);
			CancelRemove(*Slot);
			if (!Slot->bActive)
			{
				if (Slot->ImmediateReferences > 0)
					QueueAdd(Entity, *Slot, EWorldObjectCollisionAddPriority::Immediate);
				else if (Slot->PrefetchReferences > 0)
					QueueAdd(Entity, *Slot, EWorldObjectCollisionAddPriority::Regular);
				else
					ClearPendingAdd(*Slot);
			}
			return;
		}

		ClearPendingAdd(*Slot);
		if (!Slot->bActive)
		{
			DiscardIfUnused(Entity);
			return;
		}
		if (!Slot->bInGrace)
		{
			Slot->bInGrace = true;
			const uint32 Generation = ++Slot->GraceGeneration;
			GraceHeap.HeapPush(
				{NowSeconds + InConfig.GraceSeconds, Entity, Generation},
				FWorldObjectCollisionDeadlineMinHeap());
		}
	}

	void RemoveSourceMembership(FSourceSlot& Source, TSet<FWorldObjectEntityHandle>& Touched)
	{
		for (const FWorldObjectEntityHandle Entity : Source.Immediate)
		{
			if (FCollisionSlot* Slot = CollisionSlots.Find(Entity))
			{
				Slot->ImmediateReferences = FMath::Max(0, Slot->ImmediateReferences - 1);
				Touched.Add(Entity);
			}
		}
		for (const FWorldObjectEntityHandle Entity : Source.Prefetch)
		{
			if (FCollisionSlot* Slot = CollisionSlots.Find(Entity))
			{
				Slot->PrefetchReferences = FMath::Max(0, Slot->PrefetchReferences - 1);
				Touched.Add(Entity);
			}
		}
		for (const FWorldObjectEntityHandle Entity : Source.Retention)
		{
			if (FCollisionSlot* Slot = CollisionSlots.Find(Entity))
			{
				Slot->RetentionReferences = FMath::Max(0, Slot->RetentionReferences - 1);
				Touched.Add(Entity);
			}
		}
		Source.Immediate.Reset();
		Source.Prefetch.Reset();
		Source.Retention.Reset();
	}

	bool HasPendingWork() const
	{
		return DirtySourceCount > 0 || PendingAddCount > 0 || PendingRemoveCount > 0
			|| !GraceHeap.IsEmpty();
	}

	FWorldObjectCollisionActivationConfig Config;
	TArray<FSourceSlot> Sources;
	int32 FirstFreeSource = INDEX_NONE;
	int32 SourceCount = 0;
	int32 DirtySourceCount = 0;
	TMap<FWorldObjectEntityHandle, FCollisionSlot> CollisionSlots;
	TArray<FWorldObjectEntityHandle> StandardOwners;
	TArray<FWorldObjectEntityHandle> LooseDebrisOwners;
	TArray<FWorldObjectEntityHandle> PendingAdds;
	TArray<FWorldObjectEntityHandle> PendingImmediateAdds;
	TArray<FWorldObjectEntityHandle> PendingRemoves;
	TMap<FWorldObjectEntityHandle, FVector> PendingPawnContactVelocities;
	TArray<FWorldObjectCollisionGraceDeadline> GraceHeap;
	int32 PendingAddHead = 0;
	int32 PendingImmediateAddHead = 0;
	int32 PendingRemoveHead = 0;
	int32 PendingAddCount = 0;
	int32 PendingRemoveCount = 0;
	FWorldObjectCollisionStats Stats;
};

UWorldObjectCollisionWorldSubsystem::UWorldObjectCollisionWorldSubsystem() = default;
UWorldObjectCollisionWorldSubsystem::~UWorldObjectCollisionWorldSubsystem() = default;

void UWorldObjectCollisionWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Data = MakePimpl<FWorldObjectCollisionData>();
	if (UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		SnapshotBatchHandle = WorldObjects->OnQuerySnapshotBatchCommitted().AddUObject(
			this, &UWorldObjectCollisionWorldSubsystem::HandleSnapshotBatch);
	}
}

void UWorldObjectCollisionWorldSubsystem::Deinitialize()
{
	if (UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
		WorldObjects && WorldObjects->HasRuntimeState())
	{
		WorldObjects->OnQuerySnapshotBatchCommitted().Remove(SnapshotBatchHandle);
	}
	ReleaseCollisionHost();
	Data.Reset();
	Super::Deinitialize();
}

bool UWorldObjectCollisionWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UWorldObjectCollisionWorldSubsystem::IsTickable() const
{
	return Data && Data->HasPendingWork();
}

TStatId UWorldObjectCollisionWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWorldObjectCollisionWorldSubsystem, STATGROUP_Tickables);
}

const FWorldObjectCollisionActivationConfig& UWorldObjectCollisionWorldSubsystem::GetActivationConfig() const
{
	static const FWorldObjectCollisionActivationConfig Fallback;
	return Data ? Data->Config : Fallback;
}

FWorldObjectCollisionSourceHandle UWorldObjectCollisionWorldSubsystem::RegisterSource(
	const FWorldObjectCollisionSource& Source)
{
	if (!Data || !Source.IsValid()) return {};
	int32 SlotIndex = INDEX_NONE;
	if (Data->FirstFreeSource != INDEX_NONE)
	{
		SlotIndex = Data->FirstFreeSource;
		Data->FirstFreeSource = Data->Sources[SlotIndex].NextFree;
	}
	else
	{
		SlotIndex = Data->Sources.AddDefaulted();
	}
	FWorldObjectCollisionData::FSourceSlot& Entry = Data->Sources[SlotIndex];
	Entry.Source = Source;
	Entry.NextFree = INDEX_NONE;
	Entry.bAlive = true;
	Entry.bDirty = true;
	Entry.bContactSweepDirty = true;
	++Data->SourceCount;
	++Data->DirtySourceCount;
	++Data->Stats.SourceSubmitCount;
	return {SlotIndex, Entry.Generation};
}

bool UWorldObjectCollisionWorldSubsystem::UpdateSource(
	const FWorldObjectCollisionSourceHandle Handle, const FWorldObjectCollisionSource& Source)
{
	if (!Data || !Source.IsValid() || !Data->Sources.IsValidIndex(Handle.Slot)) return false;
	FWorldObjectCollisionData::FSourceSlot& Entry = Data->Sources[Handle.Slot];
	if (!Entry.bAlive || Entry.Generation != Handle.Generation) return false;
	if (AreSourcesEquivalentIgnoringRevision(Entry.Source, Source)) return true;
	Entry.Source = Source;
	Entry.bContactSweepDirty = true;
	if (!Entry.bDirty)
	{
		Entry.bDirty = true;
		++Data->DirtySourceCount;
	}
	++Data->Stats.SourceSubmitCount;
	return true;
}

bool UWorldObjectCollisionWorldSubsystem::UnregisterSource(const FWorldObjectCollisionSourceHandle Handle)
{
	if (!Data || !Data->Sources.IsValidIndex(Handle.Slot)) return false;
	FWorldObjectCollisionData::FSourceSlot& Entry = Data->Sources[Handle.Slot];
	if (!Entry.bAlive || Entry.Generation != Handle.Generation) return false;
	TSet<FWorldObjectEntityHandle> Touched;
	Data->RemoveSourceMembership(Entry, Touched);
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FWorldObjectEntityHandle Entity : Touched)
	{
		Data->ReconcileSlot(Entity, NowSeconds, Data->Config);
	}
	if (Entry.bDirty) Data->DirtySourceCount = FMath::Max(0, Data->DirtySourceCount - 1);
	const uint32 NextGeneration = Entry.Generation == MAX_uint32 ? 1 : Entry.Generation + 1;
	Entry = {};
	Entry.Generation = NextGeneration;
	Entry.NextFree = Data->FirstFreeSource;
	Data->FirstFreeSource = Handle.Slot;
	--Data->SourceCount;
	return true;
}

void UWorldObjectCollisionWorldSubsystem::HandleSnapshotBatch(
	const FWorldObjectQuerySnapshotBatch& Batch)
{
	if (!Data || Batch.Changes.IsEmpty()) return;
	CSV_SCOPED_TIMING_STAT(WorldObjectCollision, LifecycleIncrementalUpdate);
	UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!WorldObjects) return;

	// Source 没有移动时，Snapshot 已经给出精确的前后实体；直接维护 membership。
	// 旧实现把每个落地批次都升级成整片 Retention Query，陨石结算会退化成
	// O(批次数 * 已落地物件数)，并在回调里无上限同步创建碰撞实例。
	TSet<FWorldObjectEntityHandle> Touched;
	TArray<FWorldObjectEntityHandle> ContinuityHandoffs;
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FWorldObjectQuerySnapshotChange& Change : Batch.Changes)
	{
		const bool bCurrentDormant = Change.Current.IsSet()
			&& Change.Current->MotionState == EWorldObjectMotionState::Dormant;
		FTransform CollisionTransform;
		FBox CollisionBounds(ForceInit);
		EWorldObjectPhysicsCollisionPolicy Policy;
		const bool bNeedsCollisionInstance = Change.Current.IsSet()
			&& TryBuildCollisionProjection(*WorldObjects, Change.Entity, CollisionTransform, CollisionBounds, Policy);
		const bool bActiveToDormant = bCurrentDormant && Change.Previous.IsSet()
			&& Change.Previous->MotionState != EWorldObjectMotionState::Dormant
			&& WorldObjects->GetProxy(Change.Entity) != nullptr;
		const bool bTransformChanged = Change.Previous.IsSet() && Change.Current.IsSet()
			&& Change.Previous->TransformRevision != Change.Current->TransformRevision;
		if (!bNeedsCollisionInstance || bTransformChanged)
		{
			RemoveCollisionInstance(Change.Entity);
		}

		for (FWorldObjectCollisionData::FSourceSlot& Source : Data->Sources)
		{
			if (!Source.bAlive) continue;
			bool bTouched = false;
			if (Source.Immediate.Remove(Change.Entity) > 0)
			{
				if (FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Change.Entity))
				{
					Slot->ImmediateReferences = FMath::Max(0, Slot->ImmediateReferences - 1);
				}
				bTouched = true;
			}
			if (Source.Prefetch.Remove(Change.Entity) > 0)
			{
				if (FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Change.Entity))
				{
					Slot->PrefetchReferences = FMath::Max(0, Slot->PrefetchReferences - 1);
				}
				bTouched = true;
			}
			if (Source.Retention.Remove(Change.Entity) > 0)
			{
				if (FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Change.Entity))
				{
					Slot->RetentionReferences = FMath::Max(0, Slot->RetentionReferences - 1);
				}
				bTouched = true;
			}

			if (bNeedsCollisionInstance
				&& Source.Source.RetentionBounds.Intersect(CollisionBounds))
			{
				FWorldObjectCollisionData::FCollisionSlot& Slot = Data->EnsureSlot(Change.Entity);
				Slot.Policy = Policy;
				Source.Retention.Add(Change.Entity);
				++Slot.RetentionReferences;
				if (Source.Source.PrefetchBounds.Intersect(CollisionBounds))
				{
					Source.Prefetch.Add(Change.Entity);
					++Slot.PrefetchReferences;
				}
				if (Source.Source.ImmediateBounds.Intersect(CollisionBounds))
				{
					Source.Immediate.Add(Change.Entity);
					++Slot.ImmediateReferences;
				}
				bTouched = true;
			}
			if (bTouched)
			{
				Touched.Add(Change.Entity);
			}
		}
		if (bActiveToDormant)
		{
			ContinuityHandoffs.Add(Change.Entity);
		}
	}
	for (const FWorldObjectEntityHandle Entity : Touched)
	{
		Data->ReconcileSlot(Entity, NowSeconds, Data->Config);
	}

	// 自动 Physics Proxy 在 PublishShapeTransition 返回后立即退役。只对本批 Active -> Dormant
	// 且仍持有 Proxy 的实体绕过常规帧预算，按碰撞类型批量建立接管实例，避免一帧碰撞真空。
	ApplyContinuityHandoffAdds(ContinuityHandoffs);
}

void UWorldObjectCollisionWorldSubsystem::NotifyPhysicsProjectionChanged(const FWorldObjectEntityHandle Entity)
{
	if (!Data || !Entity.IsSet()) return;
	UWorldObjectWorldSubsystem* Objects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	FWorldObjectShapeInstanceSnapshot Shape;
	if (!Objects || !Objects->CopyEntityShapeSnapshot(Entity, Shape)) return;
	FWorldObjectQuerySnapshotBatch LocalChange;
	FWorldObjectQuerySnapshotChange& Change = LocalChange.Changes.AddDefaulted_GetRef();
	Change.Entity = Entity;
	Change.Previous = Shape;
	Change.Current = Shape;
	// 只驱动本地碰撞 membership；不向公共 Snapshot Stream 发布伪 Gameplay 变化。
	HandleSnapshotBatch(LocalChange);
	ApplyContinuityHandoffAdds(MakeArrayView(&Entity, 1));
}

void UWorldObjectCollisionWorldSubsystem::RefreshDirtySources()
{
	UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!Data || !WorldObjects || Data->DirtySourceCount == 0) return;
	const double NowSeconds = FPlatformTime::Seconds();
	FWorldObjectSpatialQueryScratch Scratch;
	TArray<FWorldObjectEntityHandle> Candidates;
	for (FWorldObjectCollisionData::FSourceSlot& Source : Data->Sources)
	{
		if (!Source.bAlive || !Source.bDirty) continue;
		TSet<FWorldObjectEntityHandle> Touched;
		Data->RemoveSourceMembership(Source, Touched);
		WorldObjects->QueryOverlap(Source.Source.RetentionBounds, Scratch, Candidates);
		++Data->Stats.SpatialQueryCount;
		for (const FWorldObjectEntityHandle Entity : Candidates)
		{
			++Data->Stats.CandidateTestCount;
			FTransform CollisionTransform;
			FBox CollisionBounds(ForceInit);
			EWorldObjectPhysicsCollisionPolicy Policy;
			if (!TryBuildCollisionProjection(
				*WorldObjects, Entity, CollisionTransform, CollisionBounds, Policy)
				|| !Source.Source.RetentionBounds.Intersect(CollisionBounds))
			{
				continue;
			}
			FWorldObjectCollisionData::FCollisionSlot& Slot = Data->EnsureSlot(Entity);
			Slot.Policy = Policy;
			Source.Retention.Add(Entity);
			++Slot.RetentionReferences;
			Touched.Add(Entity);
			if (Source.Source.PrefetchBounds.Intersect(CollisionBounds))
			{
				Source.Prefetch.Add(Entity);
				++Slot.PrefetchReferences;
			}
			if (Source.Source.ImmediateBounds.Intersect(CollisionBounds))
			{
				Source.Immediate.Add(Entity);
				++Slot.ImmediateReferences;
			}
			if (Source.bContactSweepDirty
				&& Policy == EWorldObjectPhysicsCollisionPolicy::LooseDebris
				&& Source.Source.PawnContactBounds.Intersect(CollisionBounds)
				&& !Source.Source.Velocity.IsNearlyZero(1.0))
			{
				// 走廊只预唤醒，不提前推动；真实接触由 CharacterMovement 的受控推速处理。
				Data->PendingPawnContactVelocities.FindOrAdd(Entity);
			}
		}
		for (const FWorldObjectEntityHandle Entity : Touched)
		{
			Data->ReconcileSlot(Entity, NowSeconds, Data->Config);
		}
		Source.bDirty = false;
		Source.bContactSweepDirty = false;
		Data->DirtySourceCount = FMath::Max(0, Data->DirtySourceCount - 1);
	}
}

void UWorldObjectCollisionWorldSubsystem::Tick(const float DeltaTime)
{
	(void)DeltaTime;
	if (!Data) return;
	CSV_SCOPED_TIMING_STAT(WorldObjectCollision, Tick);
	ProcessPendingPawnContacts();
	{
		CSV_SCOPED_TIMING_STAT(WorldObjectCollision, RefreshDirtySources);
		RefreshDirtySources();
	}
	ExpireGrace(FPlatformTime::Seconds());
	{
		CSV_SCOPED_TIMING_STAT(WorldObjectCollision, ApplyImmediateAdds);
		ApplyAdds(Data->Config.ImmediateAddsPerFrame, true);
	}
	{
		CSV_SCOPED_TIMING_STAT(WorldObjectCollision, ApplyPredictiveAdds);
		ApplyAdds(Data->Config.PredictiveAddsPerFrame, false);
	}
	ApplyRemoves(Data->Config.RemovesPerFrame);
	CSV_CUSTOM_STAT(WorldObjectCollision, CollisionInstances,
		Data->StandardOwners.Num() + Data->LooseDebrisOwners.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldObjectCollision, PendingAdds,
		Data->PendingAddCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldObjectCollision, PendingRemoves,
		Data->PendingRemoveCount, ECsvCustomStatOp::Set);
	if (Data->SourceCount == 0 && Data->CollisionSlots.IsEmpty()
		&& Data->PendingAddCount == 0 && Data->PendingRemoveCount == 0)
	{
		ReleaseCollisionHost();
	}
}

void UWorldObjectCollisionWorldSubsystem::FlushImmediateCollisionChanges()
{
	if (!Data || Data->SourceCount == 0) return;
	CSV_SCOPED_TIMING_STAT(WorldObjectCollision, ExplicitImmediateFlush);
	{
		CSV_SCOPED_TIMING_STAT(WorldObjectCollision, RefreshDirtySources);
		RefreshDirtySources();
	}
	ExpireGrace(FPlatformTime::Seconds());
	ApplyAdds(Data->Config.ImmediateAddsPerFrame, true);
	// PlayerController 在 CharacterMovement 前调用；先按稳定 Entity 激活，角色本帧不会撞上旧 Dormant Box。
	ProcessPendingPawnContacts();
}

bool UWorldObjectCollisionWorldSubsystem::QueueLooseDebrisPawnContact(
	const UPrimitiveComponent& HitComponent, const int32 InstanceIndex,
	const FVector& PawnVelocity)
{
	if (!Data || GetWorldRef().GetNetMode() == NM_Client || PawnVelocity.ContainsNaN()
		|| &HitComponent != LooseDebrisCollisionInstances.Get()
		|| !Data->LooseDebrisOwners.IsValidIndex(InstanceIndex))
	{
		return false;
	}
	const FWorldObjectEntityHandle Entity = Data->LooseDebrisOwners[InstanceIndex];
	const FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Entity);
	if (!Slot || !Slot->bActive || Slot->InstanceIndex != InstanceIndex
		|| Slot->Policy != EWorldObjectPhysicsCollisionPolicy::LooseDebris)
	{
		return false;
	}
	FVector& QueuedVelocity = Data->PendingPawnContactVelocities.FindOrAdd(Entity);
	if (PawnVelocity.SizeSquared2D() > QueuedVelocity.SizeSquared2D())
	{
		QueuedVelocity = PawnVelocity;
	}
	return true;
}

void UWorldObjectCollisionWorldSubsystem::ProcessPendingPawnContacts()
{
	if (!Data || Data->PendingPawnContactVelocities.IsEmpty()) return;
	TMap<FWorldObjectEntityHandle, FVector> Contacts = MoveTemp(Data->PendingPawnContactVelocities);
	Data->PendingPawnContactVelocities.Reset();
	UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!WorldObjects || GetWorldRef().GetNetMode() == NM_Client) return;
	for (const TPair<FWorldObjectEntityHandle, FVector>& Contact : Contacts)
	{
		const FWorldObjectPhysicsBodyFragment* Physics =
			WorldObjects->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(Contact.Key);
		const FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Contact.Key);
		if (!Physics || !Slot || !Slot->bActive
			|| Slot->Policy != EWorldObjectPhysicsCollisionPolicy::LooseDebris)
		{
			continue;
		}

		const FVector ContactVelocity =
			UE::ElementSandbox::WorldObjects::Physics::ComputePawnPushVelocity(
				Contact.Value, Physics->MassKg);
		WorldObjects->ActivatePhysics(Contact.Key, ContactVelocity);
	}
}

void UWorldObjectCollisionWorldSubsystem::ExpireGrace(const double NowSeconds)
{
	if (!Data) return;
	while (!Data->GraceHeap.IsEmpty()
		&& Data->GraceHeap.HeapTop().DeadlineSeconds <= NowSeconds)
	{
		FWorldObjectCollisionGraceDeadline Deadline;
		Data->GraceHeap.HeapPop(Deadline, FWorldObjectCollisionDeadlineMinHeap(), EAllowShrinking::No);
		FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Deadline.Entity);
		if (!Slot || !Slot->bInGrace || Slot->GraceGeneration != Deadline.Generation
			|| Slot->RetentionReferences != 0)
		{
			continue;
		}
		Slot->bInGrace = false;
		Data->QueueRemove(Deadline.Entity, *Slot);
	}
}

bool UWorldObjectCollisionWorldSubsystem::EnsureCollisionHost()
{
	if (IsValid(CollisionHost) && IsValid(StandardCollisionInstances)
		&& IsValid(LooseDebrisCollisionInstances)) return true;
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CollisionHost = GetWorldRef().SpawnActor<AActor>(Parameters);
	if (!CollisionHost) return false;
	CollisionHost->SetReplicates(false);
	USceneComponent* Root = NewObject<USceneComponent>(CollisionHost, TEXT("WorldObjectCollisionRoot"));
	CollisionHost->AddInstanceComponent(Root);
	CollisionHost->SetRootComponent(Root);
	Root->RegisterComponent();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube)
	{
		ReleaseCollisionHost();
		return false;
	}

	const auto CreateInstances = [this, Root, Cube](const FName Name, const bool bLooseDebris)
	{
		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(CollisionHost, Name);
		Instances->SetupAttachment(Root);
		Instances->SetStaticMesh(Cube);
		Instances->SetRemoveSwap();
		Instances->SetVisibility(false, true);
		Instances->SetHiddenInGame(true);
		Instances->SetCastShadow(false);
		Instances->SetCanEverAffectNavigation(false);
		Instances->SetGenerateOverlapEvents(false);
		Instances->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
		Instances->CanCharacterStepUpOn = bLooseDebris ? ECB_No : ECB_Yes;
		if (bLooseDebris)
		{
			// Dormant LooseDebris 只是角色接触前的 Authority 碰撞代理，不能成为
			// CharacterMovement Base。否则 BasedMovement 会尝试把这个瞬态 ISM
			// 作为网络对象序列化，并在实例移除时反复纠正角色位置。
			Instances->SetWalkableSlopeOverride(
				FWalkableSlopeOverride(WalkableSlope_Unwalkable, 0.0f));
			Instances->SetCollisionObjectType(ECC_PhysicsBody);
			Instances->SetCollisionResponseToAllChannels(ECR_Ignore);
			Instances->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
			Instances->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
			// 两端胶囊都必须被当前落地物件阻挡；仅 Authority 负责接触唤醒和 Chaos。
			// Client 忽略 Pawn 会把正常碰撞变成等待网络纠正，积压时可持续穿过可见木块。
			Instances->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			Instances->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		}
		Instances->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CollisionHost->AddInstanceComponent(Instances);
		Instances->RegisterComponent();
		return Instances;
	};
	StandardCollisionInstances = CreateInstances(TEXT("WorldObjectStandardCollisionISM"), false);
	LooseDebrisCollisionInstances = CreateInstances(TEXT("WorldObjectLooseDebrisCollisionISM"), true);
	return IsValid(StandardCollisionInstances) && IsValid(LooseDebrisCollisionInstances);
}

void UWorldObjectCollisionWorldSubsystem::ReleaseCollisionHost()
{
	StandardCollisionInstances = nullptr;
	LooseDebrisCollisionInstances = nullptr;
	if (IsValid(CollisionHost)) CollisionHost->Destroy();
	CollisionHost = nullptr;
}

void UWorldObjectCollisionWorldSubsystem::ApplyContinuityHandoffAdds(
	const TConstArrayView<FWorldObjectEntityHandle> Entities)
{
	if (!Data || Entities.IsEmpty()) return;
	UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!WorldObjects) return;

	TSet<FWorldObjectEntityHandle> Unique;
	TArray<FWorldObjectEntityHandle> StandardEntities;
	TArray<FTransform> StandardTransforms;
	TArray<FWorldObjectEntityHandle> LooseDebrisEntities;
	TArray<FTransform> LooseDebrisTransforms;
	for (const FWorldObjectEntityHandle Entity : Entities)
	{
		if (Unique.Contains(Entity)) continue;
		Unique.Add(Entity);
		FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Entity);
		if (!Slot || Slot->bActive || Slot->ImmediateReferences == 0
			|| Slot->RetentionReferences == 0)
		{
			continue;
		}
		FTransform CollisionTransform;
		FBox CollisionBounds(ForceInit);
		EWorldObjectPhysicsCollisionPolicy Policy;
		if (!TryBuildCollisionProjection(
			*WorldObjects, Entity, CollisionTransform, CollisionBounds, Policy))
		{
			continue;
		}
		Slot->Policy = Policy;
		if (Policy == EWorldObjectPhysicsCollisionPolicy::LooseDebris)
		{
			LooseDebrisEntities.Add(Entity);
			LooseDebrisTransforms.Add(CollisionTransform);
		}
		else
		{
			StandardEntities.Add(Entity);
			StandardTransforms.Add(CollisionTransform);
		}
	}
	if ((StandardEntities.IsEmpty() && LooseDebrisEntities.IsEmpty()) || !EnsureCollisionHost())
	{
		return;
	}
	SubmitAddBatch(StandardCollisionInstances.Get(), Data->StandardOwners,
		StandardEntities, StandardTransforms);
	SubmitAddBatch(LooseDebrisCollisionInstances.Get(), Data->LooseDebrisOwners,
		LooseDebrisEntities, LooseDebrisTransforms);
}

bool UWorldObjectCollisionWorldSubsystem::SubmitAddBatch(
	UInstancedStaticMeshComponent* Instances,
	TArray<FWorldObjectEntityHandle>& Owners,
	const TConstArrayView<FWorldObjectEntityHandle> Entities,
	const TConstArrayView<FTransform> Transforms)
{
	if (Entities.IsEmpty()) return true;
	if (!Data || !Instances || Entities.Num() != Transforms.Num()) return false;
	for (const FWorldObjectEntityHandle Entity : Entities)
	{
		if (!Data->CollisionSlots.Contains(Entity)) return false;
	}

	TArray<FTransform> MutableTransforms;
	MutableTransforms.Append(Transforms);
	Instances->PreAllocateInstancesMemory(MutableTransforms.Num());
	const int32 FirstInstanceIndex = Owners.Num();
	const TArray<int32> AddedIndices = Instances->AddInstances(
		MutableTransforms, true, true, false);
	bool bValidBatch = AddedIndices.Num() == Entities.Num();
	for (int32 Index = 0; bValidBatch && Index < AddedIndices.Num(); ++Index)
	{
		bValidBatch = AddedIndices[Index] == FirstInstanceIndex + Index;
	}
	if (!bValidBatch)
	{
		if (!AddedIndices.IsEmpty())
		{
			TArray<int32> RollbackIndices = AddedIndices;
			RollbackIndices.Sort([](const int32 Left, const int32 Right) { return Left > Right; });
			Instances->RemoveInstances(RollbackIndices, true);
		}
		return false;
	}

	Owners.Reserve(Owners.Num() + Entities.Num());
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		FWorldObjectCollisionData::FCollisionSlot& Slot =
			Data->CollisionSlots.FindChecked(Entities[Index]);
		Slot.InstanceIndex = AddedIndices[Index];
		Slot.bActive = true;
		Data->ClearPendingAdd(Slot);
		Owners.Add(Entities[Index]);
	}
	return true;
}

void UWorldObjectCollisionWorldSubsystem::ApplyAdds(const int32 Budget, const bool bImmediateOnly)
{
	if (!Data || Budget <= 0) return;
	TArray<FWorldObjectEntityHandle>& Queue = bImmediateOnly
		? Data->PendingImmediateAdds : Data->PendingAdds;
	int32& Head = bImmediateOnly ? Data->PendingImmediateAddHead : Data->PendingAddHead;
	const EWorldObjectCollisionAddPriority RequiredPriority = bImmediateOnly
		? EWorldObjectCollisionAddPriority::Immediate : EWorldObjectCollisionAddPriority::Regular;
	if (Head >= Queue.Num())
	{
		Queue.Reset();
		Head = 0;
		return;
	}
	if (!EnsureCollisionHost()) return;
	UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	TArray<FWorldObjectEntityHandle> StandardEntities;
	TArray<FTransform> StandardTransforms;
	TArray<FWorldObjectEntityHandle> LooseDebrisEntities;
	TArray<FTransform> LooseDebrisTransforms;
	StandardEntities.Reserve(Budget);
	StandardTransforms.Reserve(Budget);
	LooseDebrisEntities.Reserve(Budget);
	LooseDebrisTransforms.Reserve(Budget);
	while (StandardEntities.Num() + LooseDebrisEntities.Num() < Budget && Head < Queue.Num())
	{
		const FWorldObjectEntityHandle Entity = Queue[Head++];
		FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Entity);
		if (!Slot || Slot->QueuedAddPriority != RequiredPriority) continue;
		const bool bDesired = bImmediateOnly ? Slot->ImmediateReferences > 0 : Slot->PrefetchReferences > 0;
		if (Slot->bActive || !bDesired || Slot->RetentionReferences == 0)
		{
			Data->ClearPendingAdd(*Slot);
			continue;
		}
		FTransform CollisionTransform;
		FBox CollisionBounds(ForceInit);
		EWorldObjectPhysicsCollisionPolicy Policy;
		if (!WorldObjects || !TryBuildCollisionProjection(
			*WorldObjects, Entity, CollisionTransform, CollisionBounds, Policy))
		{
			Data->ClearPendingAdd(*Slot);
			continue;
		}
		Slot->Policy = Policy;
		if (Policy == EWorldObjectPhysicsCollisionPolicy::LooseDebris)
		{
			LooseDebrisEntities.Add(Entity);
			LooseDebrisTransforms.Add(CollisionTransform);
		}
		else
		{
			StandardEntities.Add(Entity);
			StandardTransforms.Add(CollisionTransform);
		}
	}

	if (!SubmitAddBatch(StandardCollisionInstances.Get(), Data->StandardOwners,
		StandardEntities, StandardTransforms))
	{
		Queue.Append(StandardEntities);
	}
	if (!SubmitAddBatch(LooseDebrisCollisionInstances.Get(), Data->LooseDebrisOwners,
		LooseDebrisEntities, LooseDebrisTransforms))
	{
		Queue.Append(LooseDebrisEntities);
	}
	if (Head >= Queue.Num())
	{
		Queue.Reset();
		Head = 0;
	}
}

void UWorldObjectCollisionWorldSubsystem::RemoveCollisionInstance(const FWorldObjectEntityHandle Entity)
{
	if (!Data) return;
	FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Entity);
	if (!Slot || !Slot->bActive) return;
	UInstancedStaticMeshComponent* Instances = Slot->Policy == EWorldObjectPhysicsCollisionPolicy::LooseDebris
		? LooseDebrisCollisionInstances.Get() : StandardCollisionInstances.Get();
	TArray<FWorldObjectEntityHandle>& Owners = Slot->Policy == EWorldObjectPhysicsCollisionPolicy::LooseDebris
		? Data->LooseDebrisOwners : Data->StandardOwners;
	const int32 InstanceIndex = Slot->InstanceIndex;
	if (!Instances || !Owners.IsValidIndex(InstanceIndex)) return;
	const FWorldObjectEntityHandle Moved = Owners.Last();
	if (!Instances->RemoveInstance(InstanceIndex)) return;
	if (InstanceIndex != Owners.Num() - 1)
	{
		Owners[InstanceIndex] = Moved;
		if (FWorldObjectCollisionData::FCollisionSlot* MovedSlot = Data->CollisionSlots.Find(Moved))
		{
			MovedSlot->InstanceIndex = InstanceIndex;
		}
	}
	Owners.Pop(EAllowShrinking::No);
	Slot = Data->CollisionSlots.Find(Entity);
	if (Slot)
	{
		if (Slot->bPendingRemove)
		{
			Data->PendingRemoveCount = FMath::Max(0, Data->PendingRemoveCount - 1);
		}
		Slot->bActive = false;
		Slot->InstanceIndex = INDEX_NONE;
		Slot->bPendingRemove = false;
		Slot->bInGrace = false;
	}
	Data->DiscardIfUnused(Entity);
}

void UWorldObjectCollisionWorldSubsystem::ApplyRemoves(const int32 Budget)
{
	if (!Data || Budget <= 0) return;
	int32 Applied = 0;
	while (Applied < Budget && Data->PendingRemoveHead < Data->PendingRemoves.Num())
	{
		const FWorldObjectEntityHandle Entity = Data->PendingRemoves[Data->PendingRemoveHead++];
		FWorldObjectCollisionData::FCollisionSlot* Slot = Data->CollisionSlots.Find(Entity);
		if (!Slot || !Slot->bPendingRemove) continue;
		Slot->bPendingRemove = false;
		Data->PendingRemoveCount = FMath::Max(0, Data->PendingRemoveCount - 1);
		if (Slot->bActive && Slot->RetentionReferences == 0 && !Slot->bInGrace)
		{
			RemoveCollisionInstance(Entity);
			++Applied;
		}
		else
		{
			Data->DiscardIfUnused(Entity);
		}
	}
	if (Data->PendingRemoveHead >= Data->PendingRemoves.Num())
	{
		Data->PendingRemoves.Reset();
		Data->PendingRemoveHead = 0;
	}
}

FWorldObjectCollisionStats UWorldObjectCollisionWorldSubsystem::GetStats() const
{
	FWorldObjectCollisionStats Stats;
	if (!Data) return Stats;
	Stats = Data->Stats;
	Stats.SourceCount = Data->SourceCount;
	Stats.CollisionInstanceCount = Data->StandardOwners.Num() + Data->LooseDebrisOwners.Num();
	Stats.PendingAddCount = Data->PendingAddCount;
	Stats.PendingRemoveCount = Data->PendingRemoveCount;
	return Stats;
}

bool UWorldObjectCollisionWorldSubsystem::IsIdle() const
{
	return Data && !Data->HasPendingWork();
}
