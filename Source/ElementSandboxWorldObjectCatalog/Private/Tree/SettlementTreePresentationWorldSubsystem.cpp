#include "Tree/SettlementTreePresentationWorldSubsystem.h"

#include "Async/Async.h"
#include "Components/SceneComponent.h"
#include "ElementSandboxWorldObjectCatalog.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "PresentationViewSource.h"
#include "PresentationWorldSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Subsystems/SubsystemCollection.h"
#include "Presentation/DeferredHISMComponent.h"
#include "Tree/SettlementTreeMeshFactory.h"
#include "Tree/SettlementTreeSelection.h"
#include "Tree/SettlementTreeSettings.h"
#include "Tree/SettlementTreeWorldSubsystem.h"
#include "WorldObjectWorldSubsystem.h"

CSV_DEFINE_CATEGORY(SettlementTrees, true);

namespace
{
enum class ETreeSelectionKind : uint8
{
	Local,
	Far
};

enum class ETreeReferenceKind : uint8
{
	Stable,
	Transition
};

enum class ETreeAdmissionPriority : uint8
{
	Normal,
	Emergency
};

enum class ETreeQueuedAddPriority : uint8
{
	None,
	Normal,
	Emergency
};

struct FCompactTreeView final
{
	FPresentationSourceHandle Source;
	FVector ViewLocation = FVector::ZeroVector;
	FVector SubjectLocation = FVector::ZeroVector;
	FVector HorizontalForward = FVector::ForwardVector;
	float HorizontalFOVDegrees = 90.0f;
	float AspectRatio = 16.0f / 9.0f;
	FIntPoint ViewportSize = FIntPoint(1920, 1080);

	static FCompactTreeView FromPresentation(const FPresentationViewSource& View)
	{
		FCompactTreeView Result;
		Result.Source = View.SourceHandle;
		Result.ViewLocation = View.ViewLocation;
		Result.SubjectLocation = View.SubjectLocation;
		Result.HorizontalForward = FVector(View.Forward.X, View.Forward.Y, 0.0).GetSafeNormal();
		if (Result.HorizontalForward.IsNearlyZero())
		{
			Result.HorizontalForward = FVector::ForwardVector;
		}
		Result.HorizontalFOVDegrees = View.HorizontalFOVDegrees;
		Result.AspectRatio = View.AspectRatio;
		Result.ViewportSize = View.ViewportSize;
		return Result;
	}
};

struct FTreeSelectionSet final
{
	ETreeSelectionKind Kind = ETreeSelectionKind::Local;
	FCompactTreeView Anchor;
	TMap<FWorldObjectEntityHandle, FIntPoint> Members;
	TMap<FIntPoint, TArray<FWorldObjectEntityHandle>> CellMembers;
	int32 MissingInstanceCount = 0;
	double ReadySinceSeconds = 0.0;
	bool bValid = false;

	void ResetStorage()
	{
		Members.Reset();
		CellMembers.Reset();
		MissingInstanceCount = 0;
		ReadySinceSeconds = 0.0;
		bValid = false;
	}
};

struct FTreeDeadline final
{
	double DeadlineSeconds = 0.0;
	FWorldObjectEntityHandle Entity;
	uint32 Generation = 0;
};

struct FTreeBuildDeadline final
{
	double DeadlineSeconds = 0.0;
	FIntPoint Cell = FIntPoint::ZeroValue;
	uint32 Generation = 0;
};

struct FDeadlineMinHeap final
{
	template <typename T> bool operator()(const T& Left, const T& Right) const
	{
		// UE 的 HeapPush/HeapPop 使用最小堆语义，较早到期项必须排在堆顶。
		return Left.DeadlineSeconds < Right.DeadlineSeconds;
	}
};

double SignedHorizontalAngleDegrees(const FVector& Left, const FVector& Right)
{
	const FVector2D Left2D(Left.X, Left.Y);
	const FVector2D Right2D(Right.X, Right.Y);
	if (Left2D.IsNearlyZero() || Right2D.IsNearlyZero())
	{
		return 0.0;
	}
	const FVector2D A = Left2D.GetSafeNormal();
	const FVector2D B = Right2D.GetSafeNormal();
	return FMath::RadiansToDegrees(
		FMath::Atan2(static_cast<double>(A.X * B.Y - A.Y * B.X), static_cast<double>(FVector2D::DotProduct(A, B))));
}

bool IsBoundsRelevantToSet(const FBox& Bounds, const FTreeSelectionSet& Set, const USettlementTreeSettings& Settings)
{
	if (!Set.bValid || Bounds.IsValid == 0)
	{
		return false;
	}
	if (Set.Kind == ETreeSelectionKind::Local)
	{
		const double CoverageRadius = ComputeSettlementTreeLocalCoverageRadius(
			Settings.LocalRadius, Settings.SourceMovementThreshold, Settings.FOVSafetyAngleDegrees);
		return ComputeSettlementTreeBoundsDistanceSquared2D(Bounds, Set.Anchor.SubjectLocation) <=
			   FMath::Square(CoverageRadius);
	}
	return DoesSettlementTreeBoundsIntersectHorizontalSector(
		Bounds, Set.Anchor.ViewLocation, Set.Anchor.HorizontalForward, Settings.ForwardCoverageAngleDegrees * 0.5);
}

bool IsCellRelevantToSet(const FSettlementTreeCellSnapshot& Snapshot, const FTreeSelectionSet& Set,
						 const USettlementTreeSettings& Settings)
{
	return IsBoundsRelevantToSet(Snapshot.AggregateBounds, Set, Settings);
}

bool IsCandidateRelevantToSet(const FSettlementTreeCandidate& Candidate, const FTreeSelectionSet& Set,
							  const USettlementTreeSettings& Settings)
{
	if (Set.Kind == ETreeSelectionKind::Local)
	{
		const double CoverageRadius = ComputeSettlementTreeLocalCoverageRadius(
			Settings.LocalRadius, Settings.SourceMovementThreshold, Settings.FOVSafetyAngleDegrees);
		return ComputeSettlementTreeBoundsDistanceSquared2D(Candidate.WorldBounds, Set.Anchor.SubjectLocation) <=
			   FMath::Square(CoverageRadius);
	}
	return DoesSettlementTreeBoundsIntersectHorizontalSector(Candidate.WorldBounds, Set.Anchor.ViewLocation,
															 Set.Anchor.HorizontalForward,
															 Settings.ForwardCoverageAngleDegrees * 0.5);
}
} // namespace

struct FSettlementTreeSelectionResult final
{
	FPresentationSourceHandle Source;
	FCompactTreeView RequestView;
	uint64 CatalogRevision = 0;
	bool bSelectedLocal = false;
	bool bSelectedFar = false;
	TArray<FSettlementTreeCandidate> LocalTrees;
	TArray<FSettlementTreeCandidate> FarTrees;
	int64 CandidateTests = 0;
	double Milliseconds = 0.0;
};

class FSettlementTreePresentationData final
{
public:
	struct FRenderSlot final
	{
		FWorldObjectEntityHandle Entity;
		FSettlementTreeCandidate Candidate;
		FIntPoint Cell = FIntPoint::ZeroValue;
		int32 InstanceIndex = INDEX_NONE;
		int32 ReferenceCount = 0;
		int32 StableReferenceCount = 0;
		int32 TransitionReferenceCount = 0;
		int32 EmergencyReferenceCount = 0;
		uint32 GraceGeneration = 0;
		ETreeQueuedAddPriority QueuedAddPriority = ETreeQueuedAddPriority::None;
		bool bInstanced = false;
		bool bPendingRemove = false;
		bool bInGrace = false;
		bool bCatalogRemoved = false;
		bool bCustomDataDirty = false;
	};

	struct FCluster final
	{
		UDeferredHISMComponent* Component = nullptr;
		TArray<FWorldObjectEntityHandle> Owners;
		double FirstDirtySeconds = 0.0;
		double LastDirtySeconds = 0.0;
		uint32 BuildGeneration = 0;
		uint64 ObservedBuildCount = 0;
		uint64 ObservedCoalescedCount = 0;
		bool bHasBuiltPopulatedTree = false;
	};

	struct FSourceState final
	{
		FCompactTreeView Latest;
		FVector PreviousForward = FVector::ForwardVector;
		double LastObservationSeconds = 0.0;
		double RapidSettleUntilSeconds = 0.0;
		double LastTurnSeconds = -DBL_MAX;
		double PromotionLockUntilSeconds = 0.0;
		int8 LastTurnSign = 0;
		bool bHasLatest = false;
		bool bNeedsLocalSelection = true;
		bool bNeedsFarSelection = true;
		FTreeSelectionSet ActiveLocal;
		FTreeSelectionSet TargetLocal;
		FTreeSelectionSet ActiveFar;
		FTreeSelectionSet TransitionFar;

		FSourceState()
		{
			ActiveLocal.Kind = ETreeSelectionKind::Local;
			TargetLocal.Kind = ETreeSelectionKind::Local;
			ActiveFar.Kind = ETreeSelectionKind::Far;
			TransitionFar.Kind = ETreeSelectionKind::Far;
		}
	};

	void EnsureSlot(const int32 SlotIndex)
	{
		if (SlotIndex >= 0 && Slots.Num() <= SlotIndex)
		{
			Slots.SetNum(SlotIndex + 1);
		}
	}

	FRenderSlot* FindSlot(const FWorldObjectEntityHandle Entity)
	{
		if (Slots.IsValidIndex(Entity.GetSlot()) && Slots[Entity.GetSlot()].Entity == Entity)
		{
			return &Slots[Entity.GetSlot()];
		}
		return RetiredSlots.Find(Entity);
	}

	const FRenderSlot* FindSlot(const FWorldObjectEntityHandle Entity) const
	{
		if (Slots.IsValidIndex(Entity.GetSlot()) && Slots[Entity.GetSlot()].Entity == Entity)
		{
			return &Slots[Entity.GetSlot()];
		}
		return RetiredSlots.Find(Entity);
	}

	void ClearPendingAdd(FRenderSlot& Slot)
	{
		if (Slot.QueuedAddPriority != ETreeQueuedAddPriority::None)
		{
			Slot.QueuedAddPriority = ETreeQueuedAddPriority::None;
			PendingAddCount = FMath::Max(0, PendingAddCount - 1);
		}
	}

	void QueueAdd(FRenderSlot& Slot, const ETreeAdmissionPriority Priority)
	{
		const ETreeQueuedAddPriority DesiredPriority = Priority == ETreeAdmissionPriority::Emergency
														   ? ETreeQueuedAddPriority::Emergency
														   : ETreeQueuedAddPriority::Normal;
		if (Slot.QueuedAddPriority == ETreeQueuedAddPriority::None)
		{
			Slot.QueuedAddPriority = DesiredPriority;
			++PendingAddCount;
			if (DesiredPriority == ETreeQueuedAddPriority::Emergency)
			{
				EmergencyAdds.Add(Slot.Entity);
			}
			else
			{
				NormalAdds.Add(Slot.Entity);
			}
		}
		else if (Slot.QueuedAddPriority == ETreeQueuedAddPriority::Normal &&
				 DesiredPriority == ETreeQueuedAddPriority::Emergency)
		{
			Slot.QueuedAddPriority = ETreeQueuedAddPriority::Emergency;
			EmergencyAdds.Add(Slot.Entity);
		}
	}

	void QueueCustomDataUpdate(FRenderSlot& Slot)
	{
		if (!Slot.bInstanced || Slot.bCustomDataDirty)
		{
			return;
		}
		Slot.bCustomDataDirty = true;
		++PendingCustomDataUpdateCount;
		PendingCustomDataUpdates.Add(Slot.Entity);
	}

	void ClearCustomDataUpdate(FRenderSlot& Slot)
	{
		if (!Slot.bCustomDataDirty)
		{
			return;
		}
		Slot.bCustomDataDirty = false;
		PendingCustomDataUpdateCount = FMath::Max(0, PendingCustomDataUpdateCount - 1);
	}

	void UpdateCandidate(FRenderSlot& Slot, const FSettlementTreeCandidate& Candidate)
	{
		const bool bCustomDataChanged =
			!FMath::IsNearlyEqual(Slot.Candidate.ColorVariation, Candidate.ColorVariation)
			|| !FMath::IsNearlyEqual(Slot.Candidate.BurnAmount, Candidate.BurnAmount);
		if (Slot.Candidate.WorldEntityId != Candidate.WorldEntityId
			&& Slot.Candidate.WorldEntityId.IsSet())
		{
			if (const FWorldObjectEntityHandle* Mapped = EntityByWorldEntityId.Find(
				Slot.Candidate.WorldEntityId); Mapped && *Mapped == Slot.Entity)
			{
				EntityByWorldEntityId.Remove(Slot.Candidate.WorldEntityId);
			}
		}
		Slot.Candidate = Candidate;
		Slot.Cell = Candidate.Cell;
		if (Candidate.WorldEntityId.IsSet())
		{
			EntityByWorldEntityId.Add(Candidate.WorldEntityId, Slot.Entity);
		}
		if (bCustomDataChanged)
		{
			QueueCustomDataUpdate(Slot);
		}
	}

	void CancelGrace(FRenderSlot& Slot)
	{
		if (!Slot.bInGrace)
		{
			return;
		}
		Slot.bInGrace = false;
		++Slot.GraceGeneration;
		GraceCount = FMath::Max(0, GraceCount - 1);
	}

	void DiscardSlot(const FWorldObjectEntityHandle Entity)
	{
		FRenderSlot* Slot = FindSlot(Entity);
		if (!Slot)
		{
			return;
		}
		check(Slot->ReferenceCount == 0 && !Slot->bInstanced);
		ClearPendingAdd(*Slot);
		ClearCustomDataUpdate(*Slot);
		CancelGrace(*Slot);
		if (Slot->bPendingRemove)
		{
			Slot->bPendingRemove = false;
			PendingRemoveCount = FMath::Max(0, PendingRemoveCount - 1);
		}
		if (Slot->Candidate.WorldEntityId.IsSet())
		{
			if (const FWorldObjectEntityHandle* Mapped = EntityByWorldEntityId.Find(
				Slot->Candidate.WorldEntityId); Mapped && *Mapped == Entity)
			{
				EntityByWorldEntityId.Remove(Slot->Candidate.WorldEntityId);
			}
		}
		if (Slots.IsValidIndex(Entity.GetSlot()) && Slots[Entity.GetSlot()].Entity == Entity)
		{
			Slots[Entity.GetSlot()] = {};
		}
		else
		{
			RetiredSlots.Remove(Entity);
		}
	}

	void RetireCurrentSlot(FRenderSlot& Slot)
	{
		const FWorldObjectEntityHandle RetiredEntity = Slot.Entity;
		check(RetiredEntity.IsSet() && !RetiredSlots.Contains(RetiredEntity));

		// Registry 可以在预算化 HISM Remove 完成前复用数字 Slot。旧 Generation
		// 仍由完整 Handle 的队列和 Owner 映射引用，不能被新实体覆盖。
		Slot.bCatalogRemoved = true;
		ClearPendingAdd(Slot);
		if (Slot.ReferenceCount == 0)
		{
			CancelGrace(Slot);
			if (!Slot.bInstanced)
			{
				DiscardSlot(RetiredEntity);
				return;
			}
			QueueRemove(Slot);
		}
		RetiredSlots.Add(RetiredEntity, MoveTemp(Slot));
		Slot = {};
	}

	bool Acquire(const FSettlementTreeCandidate& Candidate, const ETreeReferenceKind ReferenceKind,
				 const ETreeAdmissionPriority Priority)
	{
		EnsureSlot(Candidate.Entity.GetSlot());
		FRenderSlot& Slot = Slots[Candidate.Entity.GetSlot()];
		if (Slot.Entity.IsSet() && Slot.Entity != Candidate.Entity)
		{
			RetireCurrentSlot(Slot);
		}
		if (!Slot.Entity.IsSet())
		{
			Slot.Entity = Candidate.Entity;
		}
		UpdateCandidate(Slot, Candidate);
		Slot.bCatalogRemoved = false;
		CancelGrace(Slot);

		if (ReferenceKind == ETreeReferenceKind::Stable)
		{
			if (Slot.StableReferenceCount++ == 0)
			{
				++ActiveLogicalCount;
			}
		}
		else if (Slot.TransitionReferenceCount++ == 0)
		{
			++TransitionLogicalCount;
		}
		if (Priority == ETreeAdmissionPriority::Emergency)
		{
			++Slot.EmergencyReferenceCount;
		}
		++Slot.ReferenceCount;
		if (Slot.bPendingRemove)
		{
			Slot.bPendingRemove = false;
			PendingRemoveCount = FMath::Max(0, PendingRemoveCount - 1);
		}
		if (!Slot.bInstanced)
		{
			QueueAdd(Slot, Priority);
		}
		return Slot.bInstanced;
	}

	void QueueRemove(FRenderSlot& Slot)
	{
		if (!Slot.bInstanced || Slot.bPendingRemove)
		{
			return;
		}
		Slot.bPendingRemove = true;
		++PendingRemoveCount;
		PendingRemoves.Add(Slot.Entity);
	}

	void Release(const FWorldObjectEntityHandle Entity, const ETreeReferenceKind ReferenceKind,
				 const ETreeAdmissionPriority Priority, const double NowSeconds, const double GraceSeconds,
				 const bool bImmediate)
	{
		FRenderSlot* Slot = FindSlot(Entity);
		if (!Slot)
		{
			return;
		}
		if (ReferenceKind == ETreeReferenceKind::Stable)
		{
			check(Slot->StableReferenceCount > 0);
			if (--Slot->StableReferenceCount == 0)
			{
				ActiveLogicalCount = FMath::Max(0, ActiveLogicalCount - 1);
			}
		}
		else
		{
			check(Slot->TransitionReferenceCount > 0);
			if (--Slot->TransitionReferenceCount == 0)
			{
				TransitionLogicalCount = FMath::Max(0, TransitionLogicalCount - 1);
			}
		}
		if (Priority == ETreeAdmissionPriority::Emergency)
		{
			check(Slot->EmergencyReferenceCount > 0);
			--Slot->EmergencyReferenceCount;
		}
		check(Slot->ReferenceCount > 0);
		--Slot->ReferenceCount;
		if (Slot->ReferenceCount != 0)
		{
			return;
		}
		ClearPendingAdd(*Slot);
		if (!Slot->bInstanced)
		{
			if (Slot->bCatalogRemoved)
			{
				DiscardSlot(Entity);
			}
			return;
		}
		if (bImmediate || Slot->bCatalogRemoved || GraceSeconds <= 0.0)
		{
			CancelGrace(*Slot);
			QueueRemove(*Slot);
			return;
		}
		if (!Slot->bInGrace)
		{
			Slot->bInGrace = true;
			++GraceCount;
		}
		const uint32 Generation = ++Slot->GraceGeneration;
		GraceHeap.HeapPush({NowSeconds + GraceSeconds, Entity, Generation}, FDeadlineMinHeap());
	}

	bool AddMember(FTreeSelectionSet& Set, const FSettlementTreeCandidate& Candidate,
				   const ETreeReferenceKind ReferenceKind, const ETreeAdmissionPriority Priority)
	{
		if (Set.Members.Contains(Candidate.Entity))
		{
			if (FRenderSlot* Slot = FindSlot(Candidate.Entity))
			{
				UpdateCandidate(*Slot, Candidate);
			}
			return false;
		}
		Set.Members.Add(Candidate.Entity, Candidate.Cell);
		Set.CellMembers.FindOrAdd(Candidate.Cell).Add(Candidate.Entity);
		const bool bInstanced = Acquire(Candidate, ReferenceKind, Priority);
		if (ReferenceKind == ETreeReferenceKind::Transition && !bInstanced)
		{
			++Set.MissingInstanceCount;
			Set.ReadySinceSeconds = 0.0;
		}
		return true;
	}

	bool RemoveMember(FTreeSelectionSet& Set, const FWorldObjectEntityHandle Entity,
					  const ETreeReferenceKind ReferenceKind, const ETreeAdmissionPriority Priority,
					  const double NowSeconds, const USettlementTreeSettings& Settings, const bool bImmediate)
	{
		FIntPoint Cell;
		if (!Set.Members.RemoveAndCopyValue(Entity, Cell))
		{
			return false;
		}
		if (TArray<FWorldObjectEntityHandle>* CellEntities = Set.CellMembers.Find(Cell))
		{
			CellEntities->RemoveSingleSwap(Entity, EAllowShrinking::No);
			if (CellEntities->IsEmpty())
			{
				Set.CellMembers.Remove(Cell);
			}
		}
		const FRenderSlot* Slot = FindSlot(Entity);
		if (ReferenceKind == ETreeReferenceKind::Transition && Slot && !Slot->bInstanced)
		{
			Set.MissingInstanceCount = FMath::Max(0, Set.MissingInstanceCount - 1);
		}
		Release(Entity, ReferenceKind, Priority, NowSeconds, Settings.GraceSeconds, bImmediate);
		return true;
	}

	void ClearSet(FTreeSelectionSet& Set, const ETreeReferenceKind ReferenceKind, const ETreeAdmissionPriority Priority,
				  const double NowSeconds, const USettlementTreeSettings& Settings, const bool bImmediate = false)
	{
		TArray<FWorldObjectEntityHandle> Entities;
		Set.Members.GenerateKeyArray(Entities);
		for (const FWorldObjectEntityHandle Entity : Entities)
		{
			RemoveMember(Set, Entity, ReferenceKind, Priority, NowSeconds, Settings, bImmediate);
		}
		Set.ResetStorage();
	}

	void PopulateTarget(FTreeSelectionSet& Target, const FCompactTreeView& Anchor,
						TArray<FSettlementTreeCandidate>&& Trees, const ETreeAdmissionPriority Priority,
						const double NowSeconds, const USettlementTreeSettings& Settings)
	{
		ClearSet(Target, ETreeReferenceKind::Transition, Priority, NowSeconds, Settings);
		Target.Anchor = Anchor;
		Target.bValid = true;
		Target.Members.Reserve(Trees.Num());
		for (const FSettlementTreeCandidate& Tree : Trees)
		{
			AddMember(Target, Tree, ETreeReferenceKind::Transition, Priority);
		}
		if (Target.MissingInstanceCount == 0)
		{
			Target.ReadySinceSeconds = NowSeconds;
		}
	}

	bool ReconcileCell(FTreeSelectionSet& Set, const FSettlementTreeCellChange& Change,
					   const ETreeReferenceKind ReferenceKind, const ETreeAdmissionPriority Priority,
					   const double NowSeconds, const USettlementTreeSettings& Settings,
					   const bool bReconcileFromSnapshot = false)
	{
		if (!Set.bValid)
		{
			return false;
		}
		const bool bHadMembers = Set.CellMembers.Contains(Change.Cell);
		const bool bSnapshotRelevant = Change.Snapshot && Change.Snapshot->Shards && Change.Snapshot->TreeCount > 0 &&
									   IsCellRelevantToSet(*Change.Snapshot, Set, Settings);
		if (!bHadMembers && !bSnapshotRelevant)
		{
			return false;
		}

		++Stats.CellDeltaEvaluationCount;
		if (!bReconcileFromSnapshot)
		{
			// Cell 已消失，或 Aggregate Bounds 已完全离开本选择覆盖时，旧成员可以
			// 一次清空；其余常规注入只检查本批变化，不能重扫不断长大的 1km Cell。
			if (!bSnapshotRelevant)
			{
				TArray<FWorldObjectEntityHandle> Existing;
				if (const TArray<FWorldObjectEntityHandle>* ExistingPtr = Set.CellMembers.Find(Change.Cell))
				{
					Existing = *ExistingPtr;
				}
				for (const FWorldObjectEntityHandle Entity : Existing)
				{
					RemoveMember(Set, Entity, ReferenceKind, Priority, NowSeconds, Settings, false);
				}
				if (ReferenceKind == ETreeReferenceKind::Transition && Set.MissingInstanceCount == 0)
				{
					Set.ReadySinceSeconds = NowSeconds;
				}
				return true;
			}

			if (Change.RemovedEntities)
			{
				for (const FWorldObjectEntityHandle Entity : *Change.RemovedEntities)
				{
					RemoveMember(Set, Entity, ReferenceKind, Priority, NowSeconds, Settings, false);
				}
			}
			if (Change.UpsertedTrees)
			{
				for (const FSettlementTreeCandidate& Candidate : *Change.UpsertedTrees)
				{
					++Stats.CandidateTestCount;
					if (IsCandidateRelevantToSet(Candidate, Set, Settings))
					{
						AddMember(Set, Candidate, ReferenceKind, Priority);
					}
					else
					{
						RemoveMember(Set, Candidate.Entity, ReferenceKind, Priority, NowSeconds, Settings, false);
					}
				}
			}
			if (ReferenceKind == ETreeReferenceKind::Transition && Set.MissingInstanceCount == 0)
			{
				Set.ReadySinceSeconds = NowSeconds;
			}
			return true;
		}

		// Worker 的基础快照可能早于内容注入；返回时只对每个受影响 Cell 用最后一份
		// 不可变快照追一次，避免丢失 Worker 在途期间同 Cell 的多批变化。
		TMap<FWorldObjectEntityHandle, const FSettlementTreeCandidate*> Desired;
		if (bSnapshotRelevant)
		{
			Desired.Reserve(Change.Snapshot->TreeCount);
			for (const FSettlementTreeSnapshotShard& Shard : *Change.Snapshot->Shards)
			{
				if (!Shard.Trees || !IsBoundsRelevantToSet(Shard.AggregateBounds, Set, Settings))
				{
					continue;
				}
				for (const FSettlementTreeCandidate& Candidate : *Shard.Trees)
				{
					++Stats.CandidateTestCount;
					if (IsCandidateRelevantToSet(Candidate, Set, Settings))
					{
						Desired.Add(Candidate.Entity, &Candidate);
					}
				}
			}
		}

		TArray<FWorldObjectEntityHandle> Existing;
		if (const TArray<FWorldObjectEntityHandle>* ExistingPtr = Set.CellMembers.Find(Change.Cell))
		{
			Existing = *ExistingPtr;
		}
		for (const FWorldObjectEntityHandle Entity : Existing)
		{
			if (!Desired.Contains(Entity))
			{
				RemoveMember(Set, Entity, ReferenceKind, Priority, NowSeconds, Settings, false);
			}
		}
		for (const TPair<FWorldObjectEntityHandle, const FSettlementTreeCandidate*>& Pair : Desired)
		{
			AddMember(Set, *Pair.Value, ReferenceKind, Priority);
		}
		if (ReferenceKind == ETreeReferenceKind::Transition && Set.MissingInstanceCount == 0)
		{
			Set.ReadySinceSeconds = NowSeconds;
		}
		return true;
	}

	void OnInstanceAdded(const FWorldObjectEntityHandle Entity, const double NowSeconds)
	{
		for (TPair<FPresentationSourceHandle, FSourceState>& Pair : Sources)
		{
			FTreeSelectionSet* Targets[] = {&Pair.Value.TargetLocal, &Pair.Value.TransitionFar};
			for (FTreeSelectionSet* Target : Targets)
			{
				if (Target->bValid && Target->Members.Contains(Entity) && Target->MissingInstanceCount > 0)
				{
					--Target->MissingInstanceCount;
					if (Target->MissingInstanceCount == 0)
					{
						Target->ReadySinceSeconds = NowSeconds;
					}
				}
			}
		}
	}

	/** 只审计即将提交的单个实例，不改变引用或掩盖闪烁。 */
	bool AuditCurrentViewRemoval(const FRenderSlot& Slot, const USettlementTreeSettings& Settings)
	{
		// Catalog 已明确移除的实体即使仍在画面内也必须离场；这个计数只抓
		// 选择/引用状态机错误地删除仍属于 Catalog 的可见实例。
		if (Slot.ReferenceCount != 0 || Slot.bCatalogRemoved || Slot.Candidate.WorldBounds.IsValid == 0)
		{
			return false;
		}
		const double LocalRadiusSquared = FMath::Square(Settings.LocalRadius);
		for (const TPair<FPresentationSourceHandle, FSourceState>& Pair : Sources)
		{
			const FSourceState& Source = Pair.Value;
			if (!Source.bHasLatest)
			{
				continue;
			}
			const double DistanceSquared =
				ComputeSettlementTreeBoundsDistanceSquared2D(Slot.Candidate.WorldBounds, Source.Latest.SubjectLocation);
			const bool bInsideLocalCore = DistanceSquared <= LocalRadiusSquared;
			const bool bInsideVisibleSector = DoesSettlementTreeBoundsIntersectHorizontalSector(
				Slot.Candidate.WorldBounds, Source.Latest.ViewLocation, Source.Latest.HorizontalForward,
				FMath::Clamp(Source.Latest.HorizontalFOVDegrees * 0.5f, 1.0f, 89.0f));
			if (!bInsideLocalCore && !bInsideVisibleSector)
			{
				continue;
			}
			++Stats.InvalidVisibleRemovalCount;
			UE_LOG(LogElementSandboxWorldObjectCatalog, Error,
				   TEXT("Tree visual invariant failed before HISM remove: slot=%d "
						"generation=%u distance=%.2fm local=%d visible=%d "
						"catalogRemoved=%d."),
				   Slot.Entity.GetSlot(), Slot.Entity.GetGeneration(), FMath::Sqrt(DistanceSquared) * 0.01,
				   bInsideLocalCore ? 1 : 0, bInsideVisibleSector ? 1 : 0, Slot.bCatalogRemoved ? 1 : 0);
			return true;
		}
		return false;
	}

	void PromoteSet(FTreeSelectionSet& Active, FTreeSelectionSet& Target, const ETreeAdmissionPriority Priority,
					const double NowSeconds, const USettlementTreeSettings& Settings)
	{
		if (!Target.bValid)
		{
			return;
		}
		TArray<FSettlementTreeCandidate> TargetCandidates;
		TargetCandidates.Reserve(Target.Members.Num());
		for (const TPair<FWorldObjectEntityHandle, FIntPoint>& Pair : Target.Members)
		{
			if (const FRenderSlot* Slot = FindSlot(Pair.Key))
			{
				TargetCandidates.Add(Slot->Candidate);
			}
		}
		for (const FSettlementTreeCandidate& Candidate : TargetCandidates)
		{
			Acquire(Candidate, ETreeReferenceKind::Stable, Priority);
		}
		ClearSet(Active, ETreeReferenceKind::Stable, Priority, NowSeconds, Settings);
		for (const FSettlementTreeCandidate& Candidate : TargetCandidates)
		{
			RemoveMember(Target, Candidate.Entity, ETreeReferenceKind::Transition, Priority, NowSeconds, Settings,
						 false);
		}
		Active.Anchor = Target.Anchor;
		Active.bValid = true;
		for (const FSettlementTreeCandidate& Candidate : TargetCandidates)
		{
			Active.Members.Add(Candidate.Entity, Candidate.Cell);
			Active.CellMembers.FindOrAdd(Candidate.Cell).Add(Candidate.Entity);
		}
		Target.ResetStorage();
	}

	void MarkClusterEdited(const FIntPoint Cell, FCluster& Cluster, const double NowSeconds,
						   const USettlementTreeSettings& Settings)
	{
		if (Cluster.FirstDirtySeconds <= 0.0)
		{
			Cluster.FirstDirtySeconds = NowSeconds;
		}
		Cluster.LastDirtySeconds = NowSeconds;
		// 首轮流式注入不按 1 秒上限反复重建同一个 Cell；实例数据已走原生增量路径，
		// 等该 Cell 真正静默后只建一次完整树。已有稳定树后的运行期修改仍受最大推迟保护。
		const double Deadline = Cluster.bHasBuiltPopulatedTree
			? FMath::Min(NowSeconds + Settings.TreeBuildQuietSeconds,
				Cluster.FirstDirtySeconds + Settings.TreeBuildMaxDeferralSeconds)
			: NowSeconds + Settings.TreeBuildQuietSeconds;
		const uint32 Generation = ++Cluster.BuildGeneration;
		BuildHeap.HeapPush({Deadline, Cell, Generation}, FDeadlineMinHeap());
		const uint64 Coalesced = Cluster.Component ? Cluster.Component->GetCoalescedTreeBuildCount() : 0;
		if (Coalesced >= Cluster.ObservedCoalescedCount)
		{
			Stats.CoalescedTreeBuildCount += static_cast<int64>(Coalesced - Cluster.ObservedCoalescedCount);
		}
		Cluster.ObservedCoalescedCount = Coalesced;
	}

	bool HasPendingWork(const double NowSeconds) const
	{
		if (bSelectionWorkerInFlight || PendingAddCount > 0 || PendingRemoveCount > 0
			|| PendingCustomDataUpdateCount > 0 || !GraceHeap.IsEmpty() ||
			!BuildHeap.IsEmpty() || InFlightBuildCell.IsSet() || !PendingEmptyClusters.IsEmpty())
		{
			return true;
		}
		for (const TPair<FPresentationSourceHandle, FSourceState>& Pair : Sources)
		{
			const FSourceState& Source = Pair.Value;
			if (Source.bNeedsLocalSelection || Source.bNeedsFarSelection || Source.TargetLocal.bValid ||
				Source.TransitionFar.bValid || Source.RapidSettleUntilSeconds > NowSeconds)
			{
				return true;
			}
		}
		return false;
	}

	TMap<FPresentationSourceHandle, FSourceState> Sources;
	TMap<FIntPoint, FSettlementTreeCellChange> LatestCellChanges;
	TArray<FRenderSlot> Slots;
	TMap<FWorldObjectEntityHandle, FRenderSlot> RetiredSlots;
	TMap<FWorldEntityId, FWorldObjectEntityHandle> EntityByWorldEntityId;
	TMap<FIntPoint, FCluster> Clusters;
	TArray<FWorldObjectEntityHandle> EmergencyAdds;
	TArray<FWorldObjectEntityHandle> NormalAdds;
	TArray<FWorldObjectEntityHandle> PendingRemoves;
	TArray<FWorldObjectEntityHandle> PendingCustomDataUpdates;
	TArray<FIntPoint> PendingEmptyClusters;
	TArray<FTreeDeadline> GraceHeap;
	TArray<FTreeBuildDeadline> BuildHeap;
	TOptional<FIntPoint> InFlightBuildCell;
	int32 EmergencyAddHead = 0;
	int32 NormalAddHead = 0;
	int32 PendingRemoveHead = 0;
	int32 PendingCustomDataUpdateHead = 0;
	int32 PendingAddCount = 0;
	int32 PendingRemoveCount = 0;
	int32 PendingCustomDataUpdateCount = 0;
	int32 ResidentInstanceCount = 0;
	int32 ActiveLogicalCount = 0;
	int32 TransitionLogicalCount = 0;
	int32 GraceCount = 0;
	int32 DynamicChangeBudget = 16384;
	bool bSelectionWorkerInFlight = false;
	FSettlementTreePresentationStats Stats;
};

USettlementTreePresentationWorldSubsystem::USettlementTreePresentationWorldSubsystem() = default;
USettlementTreePresentationWorldSubsystem::~USettlementTreePresentationWorldSubsystem() = default;

void USettlementTreePresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USettlementTreeWorldSubsystem>();
	Collection.InitializeDependency<UPresentationWorldSubsystem>();
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Data = MakePimpl<FSettlementTreePresentationData>();
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	Data->DynamicChangeBudget = Settings
									? FMath::Clamp(Settings->InitialChangesPerCycle, Settings->MinimumChangesPerCycle,
												   Settings->MaximumChangesPerCycle)
									: 16384;
	if (GetWorldRef().GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	if (UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		RuntimeEvictedHandle = WorldObjects->OnEntitiesRuntimeEvicted().AddUObject(
			this, &USettlementTreePresentationWorldSubsystem::QueueRemovedTrees);
		GameplayDestroyedHandle = WorldObjects->OnEntitiesGameplayDestroyed().AddUObject(
			this, &USettlementTreePresentationWorldSubsystem::QueueRemovedTrees);
	}
	if (USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		CellsPublishedHandle = Catalog->OnCellsPublished().AddUObject(
			this, &USettlementTreePresentationWorldSubsystem::HandleCellsPublished);
	}
	if (UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>())
	{
		ViewSourceUpdatedHandle = Presentation->OnViewSourceUpdated().AddUObject(
			this, &USettlementTreePresentationWorldSubsystem::HandleViewSourceUpdated);
		ViewSourceRemovedHandle = Presentation->OnViewSourceRemoved().AddUObject(
			this, &USettlementTreePresentationWorldSubsystem::HandleViewSourceRemoved);
		FPresentationViewSnapshot Snapshot;
		if (Presentation->CopyCurrentViewSnapshot(Snapshot))
		{
			for (const FPresentationViewSource& Source : Snapshot.Sources)
			{
				HandleViewSourceUpdated(Source);
			}
		}
	}
	EnsureRenderHost();
}

void USettlementTreePresentationWorldSubsystem::Deinitialize()
{
	if (UWorldObjectWorldSubsystem* WorldObjects = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		WorldObjects->OnEntitiesRuntimeEvicted().Remove(RuntimeEvictedHandle);
		WorldObjects->OnEntitiesGameplayDestroyed().Remove(GameplayDestroyedHandle);
	}
	if (USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		Catalog->OnCellsPublished().Remove(CellsPublishedHandle);
	}
	if (UPresentationWorldSubsystem* Presentation = GetWorldRef().GetSubsystem<UPresentationWorldSubsystem>())
	{
		Presentation->OnViewSourceUpdated().Remove(ViewSourceUpdatedHandle);
		Presentation->OnViewSourceRemoved().Remove(ViewSourceRemovedHandle);
	}
	ReleaseRenderHost();
	Data.Reset();
	Super::Deinitialize();
}

bool USettlementTreePresentationWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool USettlementTreePresentationWorldSubsystem::IsTickable() const
{
	return Data && GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer &&
		   Data->HasPendingWork(FPlatformTime::Seconds());
}

TStatId USettlementTreePresentationWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USettlementTreePresentationWorldSubsystem, STATGROUP_Tickables);
}

void USettlementTreePresentationWorldSubsystem::HandleViewSourceUpdated(const FPresentationViewSource& View)
{
	if (!Data || !View.IsValid() || GetWorldRef().GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	const double NowSeconds = Start;
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	FSettlementTreePresentationData::FSourceState& Source = Data->Sources.FindOrAdd(View.SourceHandle);
	const FCompactTreeView Compact = FCompactTreeView::FromPresentation(View);

	if (Source.bHasLatest && Source.LastObservationSeconds > 0.0)
	{
		const double DeltaSeconds = FMath::Max(UE_DOUBLE_SMALL_NUMBER, NowSeconds - Source.LastObservationSeconds);
		const double SignedTurn = SignedHorizontalAngleDegrees(Source.PreviousForward, Compact.HorizontalForward);
		const double AngularSpeed = FMath::Abs(SignedTurn) / DeltaSeconds;
		const int8 TurnSign = SignedTurn > UE_DOUBLE_SMALL_NUMBER ? 1 : (SignedTurn < -UE_DOUBLE_SMALL_NUMBER ? -1 : 0);
		if (AngularSpeed >= Settings->RapidRotationThresholdDegreesPerSecond)
		{
			Source.RapidSettleUntilSeconds = NowSeconds + Settings->RapidRotationSettleSeconds;
		}
		if (TurnSign != 0)
		{
			if (Source.LastTurnSign != 0 && TurnSign != Source.LastTurnSign &&
				NowSeconds - Source.LastTurnSeconds <= Settings->RotationReversalWindowSeconds)
			{
				Source.PromotionLockUntilSeconds =
					FMath::Max(Source.PromotionLockUntilSeconds, NowSeconds + Settings->UnstablePromotionLockSeconds);
			}
			Source.LastTurnSign = TurnSign;
			Source.LastTurnSeconds = NowSeconds;
		}
	}
	Source.PreviousForward = Compact.HorizontalForward;
	Source.LastObservationSeconds = NowSeconds;
	Source.Latest = Compact;
	Source.bHasLatest = true;

	const auto IsPositionCovered = [Settings](const FTreeSelectionSet& Set, const FCompactTreeView& Latest)
	{
		return Set.bValid && IsSettlementTreePositionCoverageReusable(
								 Set.Anchor.SubjectLocation, Set.Anchor.ViewLocation, Latest.SubjectLocation,
								 Latest.ViewLocation, Settings->SourceMovementThreshold);
	};
	const auto IsFarCovered =
		[Settings, &IsPositionCovered](const FTreeSelectionSet& Set, const FCompactTreeView& Latest)
	{
		return Set.bValid && IsSettlementTreeFarCoverageReusable(
								 Set.Anchor.SubjectLocation, Set.Anchor.ViewLocation, Set.Anchor.HorizontalForward,
								 Set.Anchor.HorizontalFOVDegrees, Set.Anchor.AspectRatio, Set.Anchor.ViewportSize,
								 Latest.SubjectLocation, Latest.ViewLocation, Latest.HorizontalForward,
								 Latest.HorizontalFOVDegrees, Latest.AspectRatio, Latest.ViewportSize,
								 Settings->SourceMovementThreshold, Settings->ForwardCoverageAngleDegrees,
								 Settings->FOVSafetyAngleDegrees, Settings->MinimumRecenterAngleDegrees);
	};
	Source.bNeedsLocalSelection =
		!IsPositionCovered(Source.TargetLocal, Compact) && !IsPositionCovered(Source.ActiveLocal, Compact);
	Source.bNeedsFarSelection =
		!IsFarCovered(Source.TransitionFar, Compact) && !IsFarCovered(Source.ActiveFar, Compact);

	Data->Stats.LastObservationMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	DispatchPendingSelections(NowSeconds);
}

void USettlementTreePresentationWorldSubsystem::HandleViewSourceRemoved(const FPresentationSourceHandle SourceHandle)
{
	if (!Data)
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	FSettlementTreePresentationData::FSourceState* Source = Data->Sources.Find(SourceHandle);
	if (!Source)
	{
		return;
	}
	Data->ClearSet(Source->ActiveLocal, ETreeReferenceKind::Stable, ETreeAdmissionPriority::Emergency, NowSeconds,
				   *Settings, true);
	Data->ClearSet(Source->TargetLocal, ETreeReferenceKind::Transition, ETreeAdmissionPriority::Emergency, NowSeconds,
				   *Settings, true);
	Data->ClearSet(Source->ActiveFar, ETreeReferenceKind::Stable, ETreeAdmissionPriority::Normal, NowSeconds,
				   *Settings);
	Data->ClearSet(Source->TransitionFar, ETreeReferenceKind::Transition, ETreeAdmissionPriority::Normal, NowSeconds,
				   *Settings);
	Data->Sources.Remove(SourceHandle);
}

void USettlementTreePresentationWorldSubsystem::HandleCellsPublished(
	const TConstArrayView<FSettlementTreeCellChange> Changes)
{
	if (!Data || Changes.IsEmpty())
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FSettlementTreeCellChange& Change : Changes)
	{
		Data->LatestCellChanges.Add(Change.Cell, Change);
		for (TPair<FPresentationSourceHandle, FSettlementTreePresentationData::FSourceState>& Pair : Data->Sources)
		{
			FSettlementTreePresentationData::FSourceState& Source = Pair.Value;
			Data->ReconcileCell(Source.ActiveLocal, Change, ETreeReferenceKind::Stable,
								ETreeAdmissionPriority::Emergency, NowSeconds, *Settings);
			Data->ReconcileCell(Source.ActiveFar, Change, ETreeReferenceKind::Stable, ETreeAdmissionPriority::Normal,
								NowSeconds, *Settings);
			Data->ReconcileCell(Source.TargetLocal, Change, ETreeReferenceKind::Transition,
								ETreeAdmissionPriority::Emergency, NowSeconds, *Settings);
			Data->ReconcileCell(Source.TransitionFar, Change, ETreeReferenceKind::Transition,
								ETreeAdmissionPriority::Normal, NowSeconds, *Settings);
		}
	}
	DispatchPendingSelections(NowSeconds);
}

void USettlementTreePresentationWorldSubsystem::DispatchPendingSelections(const double NowSeconds)
{
	if (!Data || Data->bSelectionWorkerInFlight)
	{
		return;
	}
	USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>();
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	if (!Catalog || !Settings)
	{
		return;
	}
	for (TPair<FPresentationSourceHandle, FSettlementTreePresentationData::FSourceState>& Pair : Data->Sources)
	{
		FSettlementTreePresentationData::FSourceState& Source = Pair.Value;
		const bool bSelectLocal = Source.bHasLatest && Source.bNeedsLocalSelection;
		const bool bSelectFar =
			Source.bHasLatest && Source.bNeedsFarSelection && NowSeconds >= Source.RapidSettleUntilSeconds;
		if (!bSelectLocal && !bSelectFar)
		{
			continue;
		}

		const double Start = FPlatformTime::Seconds();
		TArray<FSettlementTreeCellSnapshot> Cells;
		uint64 CatalogRevision = 0;
		Catalog->CopyCellSnapshots(Cells, CatalogRevision);
		TUniquePtr<FSettlementTreeSelectionResult> Request = MakeUnique<FSettlementTreeSelectionResult>();
		Request->Source = Pair.Key;
		Request->RequestView = Source.Latest;
		Request->CatalogRevision = CatalogRevision;
		Request->bSelectedLocal = bSelectLocal;
		Request->bSelectedFar = bSelectFar;
		Data->bSelectionWorkerInFlight = true;
		++Data->Stats.WorkerDispatchCount;
		Data->Stats.LocalSelectionPassCount += bSelectLocal ? 1 : 0;
		Data->Stats.FarSelectionPassCount += bSelectFar ? 1 : 0;
		Data->Stats.LastSelectionMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;

		const double LocalCoverageRadius = ComputeSettlementTreeLocalCoverageRadius(
			Settings->LocalRadius, Settings->SourceMovementThreshold, Settings->FOVSafetyAngleDegrees);
		const double LocalCoverageSquared = FMath::Square(LocalCoverageRadius);
		const double FarHalfAngle = Settings->ForwardCoverageAngleDegrees * 0.5;
		TWeakObjectPtr<USettlementTreePresentationWorldSubsystem> WeakThis(this);
		Async(EAsyncExecution::ThreadPool,
			  [WeakThis, LocalCoverageSquared, FarHalfAngle, Cells = MoveTemp(Cells),
			   Result = MoveTemp(Request)]() mutable
			  {
				  const double WorkerStart = FPlatformTime::Seconds();
				  for (const FSettlementTreeCellSnapshot& Cell : Cells)
				  {
					  if (!Cell.Shards || Cell.TreeCount == 0)
					  {
						  continue;
					  }
					  bool bLocalCell = false;
					  bool bFarCell = false;
					  if (Result->bSelectedLocal)
					  {
						  bLocalCell =
							  ComputeSettlementTreeBoundsDistanceSquared2D(
								  Cell.AggregateBounds, Result->RequestView.SubjectLocation) <= LocalCoverageSquared;
					  }
					  if (Result->bSelectedFar)
					  {
						  bFarCell = DoesSettlementTreeBoundsIntersectHorizontalSector(
							  Cell.AggregateBounds, Result->RequestView.ViewLocation,
							  Result->RequestView.HorizontalForward, FarHalfAngle);
					  }
					  if (!bLocalCell && !bFarCell)
					  {
						  continue;
					  }
					  for (const FSettlementTreeSnapshotShard& Shard : *Cell.Shards)
					  {
						  if (!Shard.Trees || Shard.Trees->IsEmpty())
						  {
							  continue;
						  }
						  const bool bLocalShard =
							  bLocalCell &&
							  ComputeSettlementTreeBoundsDistanceSquared2D(
								  Shard.AggregateBounds, Result->RequestView.SubjectLocation) <= LocalCoverageSquared;
						  const bool bFarShard =
							  bFarCell && DoesSettlementTreeBoundsIntersectHorizontalSector(
											  Shard.AggregateBounds, Result->RequestView.ViewLocation,
											  Result->RequestView.HorizontalForward, FarHalfAngle);
						  if (!bLocalShard && !bFarShard)
						  {
							  continue;
						  }
						  for (const FSettlementTreeCandidate& Candidate : *Shard.Trees)
						  {
							  if (bLocalShard)
							  {
								  ++Result->CandidateTests;
								  if (ComputeSettlementTreeBoundsDistanceSquared2D(
										  Candidate.WorldBounds, Result->RequestView.SubjectLocation) <=
									  LocalCoverageSquared)
								  {
									  Result->LocalTrees.Add(Candidate);
								  }
							  }
							  if (bFarShard)
							  {
								  ++Result->CandidateTests;
								  if (DoesSettlementTreeBoundsIntersectHorizontalSector(
										  Candidate.WorldBounds, Result->RequestView.ViewLocation,
										  Result->RequestView.HorizontalForward, FarHalfAngle))
								  {
									  Result->FarTrees.Add(Candidate);
								  }
							  }
						  }
					  }
				  }
				  Result->Milliseconds = (FPlatformTime::Seconds() - WorkerStart) * 1000.0;
				  AsyncTask(ENamedThreads::GameThread,
							[WeakThis, Result = MoveTemp(Result)]() mutable
							{
								if (USettlementTreePresentationWorldSubsystem* Self = WeakThis.Get())
								{
									Self->AcceptSelectionResult(MoveTemp(Result));
								}
							});
			  });
		return;
	}
}

void USettlementTreePresentationWorldSubsystem::AcceptSelectionResult(
	TUniquePtr<FSettlementTreeSelectionResult>&& Result)
{
	if (!Data || !Result)
	{
		return;
	}
	Data->bSelectionWorkerInFlight = false;
	Data->Stats.LastSelectionWorkerMilliseconds = Result->Milliseconds;
	Data->Stats.CandidateTestCount += Result->CandidateTests;
	FSettlementTreePresentationData::FSourceState* Source = Data->Sources.Find(Result->Source);
	if (!Source || !Source->bHasLatest)
	{
		++Data->Stats.WorkerDiscardCount;
		DispatchPendingSelections(FPlatformTime::Seconds());
		return;
	}

	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	const bool bPositionCurrent = IsSettlementTreePositionCoverageReusable(
		Result->RequestView.SubjectLocation, Result->RequestView.ViewLocation, Source->Latest.SubjectLocation,
		Source->Latest.ViewLocation, Settings->SourceMovementThreshold);
	const bool bFarCurrent = IsSettlementTreeFarCoverageReusable(
		Result->RequestView.SubjectLocation, Result->RequestView.ViewLocation, Result->RequestView.HorizontalForward,
		Result->RequestView.HorizontalFOVDegrees, Result->RequestView.AspectRatio, Result->RequestView.ViewportSize,
		Source->Latest.SubjectLocation, Source->Latest.ViewLocation, Source->Latest.HorizontalForward,
		Source->Latest.HorizontalFOVDegrees, Source->Latest.AspectRatio, Source->Latest.ViewportSize,
		Settings->SourceMovementThreshold, Settings->ForwardCoverageAngleDegrees, Settings->FOVSafetyAngleDegrees,
		Settings->MinimumRecenterAngleDegrees);

	bool bAcceptedAny = false;
	if (Result->bSelectedLocal && bPositionCurrent)
	{
		Data->PopulateTarget(Source->TargetLocal, Result->RequestView, MoveTemp(Result->LocalTrees),
							 ETreeAdmissionPriority::Emergency, NowSeconds, *Settings);
		bAcceptedAny = true;
		Source->bNeedsLocalSelection = false;
	}
	if (Result->bSelectedFar && bFarCurrent)
	{
		Data->PopulateTarget(Source->TransitionFar, Result->RequestView, MoveTemp(Result->FarTrees),
							 ETreeAdmissionPriority::Normal, NowSeconds, *Settings);
		bAcceptedAny = true;
		Source->bNeedsFarSelection = false;
	}
	if (!bAcceptedAny)
	{
		++Data->Stats.WorkerDiscardCount;
	}

	// Worker 期间的内容变化按 Cell 重放；无关 Cell 不使整份结果过期。
	for (const TPair<FIntPoint, FSettlementTreeCellChange>& Pair : Data->LatestCellChanges)
	{
		if (Pair.Value.Revision <= Result->CatalogRevision)
		{
			continue;
		}
		if (Result->bSelectedLocal && bPositionCurrent)
		{
			Data->ReconcileCell(Source->TargetLocal, Pair.Value, ETreeReferenceKind::Transition,
								ETreeAdmissionPriority::Emergency, NowSeconds, *Settings, true);
		}
		if (Result->bSelectedFar && bFarCurrent)
		{
			Data->ReconcileCell(Source->TransitionFar, Pair.Value, ETreeReferenceKind::Transition,
								ETreeAdmissionPriority::Normal, NowSeconds, *Settings, true);
		}
	}
	DispatchPendingSelections(NowSeconds);
}

void USettlementTreePresentationWorldSubsystem::Tick(const float DeltaTime)
{
	(void)DeltaTime;
	if (!Data)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(SettlementTrees_Tick);
	const double TickStart = FPlatformTime::Seconds();
	const double NowSeconds = TickStart;
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();

	DispatchPendingSelections(NowSeconds);
	ApplyPendingChanges(NowSeconds);

	for (TPair<FPresentationSourceHandle, FSettlementTreePresentationData::FSourceState>& Pair : Data->Sources)
	{
		FSettlementTreePresentationData::FSourceState& Source = Pair.Value;
		if (NowSeconds < Source.PromotionLockUntilSeconds)
		{
			continue;
		}
		if (Source.TargetLocal.bValid && Source.TargetLocal.MissingInstanceCount == 0 &&
			Source.TargetLocal.ReadySinceSeconds > 0.0 &&
			NowSeconds - Source.TargetLocal.ReadySinceSeconds >= Settings->PromotionStableSeconds)
		{
			Data->PromoteSet(Source.ActiveLocal, Source.TargetLocal, ETreeAdmissionPriority::Emergency, NowSeconds,
							 *Settings);
		}
		if (Source.TransitionFar.bValid && Source.TransitionFar.MissingInstanceCount == 0 &&
			Source.TransitionFar.ReadySinceSeconds > 0.0 &&
			NowSeconds - Source.TransitionFar.ReadySinceSeconds >= Settings->PromotionStableSeconds)
		{
			Data->PromoteSet(Source.ActiveFar, Source.TransitionFar, ETreeAdmissionPriority::Normal, NowSeconds,
							 *Settings);
		}
	}
	ProcessDeferredTreeBuilds(NowSeconds);
	CSV_CUSTOM_STAT(SettlementTrees, PresentationTickMilliseconds, (FPlatformTime::Seconds() - TickStart) * 1000.0,
					ECsvCustomStatOp::Set);
}

void USettlementTreePresentationWorldSubsystem::QueueRemovedTrees(
	const TConstArrayView<FWorldObjectLifecycleRecord> Records)
{
	if (!Data)
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FWorldObjectLifecycleRecord& Record : Records)
	{
		if (Record.DefinitionId != SettlementTreeDefinitionId)
		{
			continue;
		}
		for (TPair<FPresentationSourceHandle, FSettlementTreePresentationData::FSourceState>& Pair : Data->Sources)
		{
			FSettlementTreePresentationData::FSourceState& Source = Pair.Value;
			Data->RemoveMember(Source.ActiveLocal, Record.Entity, ETreeReferenceKind::Stable,
							   ETreeAdmissionPriority::Emergency, NowSeconds, *Settings, true);
			Data->RemoveMember(Source.TargetLocal, Record.Entity, ETreeReferenceKind::Transition,
							   ETreeAdmissionPriority::Emergency, NowSeconds, *Settings, true);
			Data->RemoveMember(Source.ActiveFar, Record.Entity, ETreeReferenceKind::Stable,
							   ETreeAdmissionPriority::Normal, NowSeconds, *Settings, true);
			Data->RemoveMember(Source.TransitionFar, Record.Entity, ETreeReferenceKind::Transition,
							   ETreeAdmissionPriority::Normal, NowSeconds, *Settings, true);
		}
		if (FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Record.Entity))
		{
			Slot->bCatalogRemoved = true;
			Data->ClearPendingAdd(*Slot);
			if (Slot->ReferenceCount == 0)
			{
				Data->CancelGrace(*Slot);
				if (Slot->bInstanced)
				{
					Data->QueueRemove(*Slot);
				}
					else
					{
						Data->DiscardSlot(Record.Entity);
					}
			}
		}
	}
}

bool USettlementTreePresentationWorldSubsystem::EnsureRenderHost()
{
	if (IsValid(RenderHost) && IsValid(TreeMesh))
	{
		return true;
	}
	if (GetWorldRef().GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RenderHost = GetWorldRef().SpawnActor<AActor>(Parameters);
	if (!RenderHost)
	{
		return false;
	}
	RenderHost->SetReplicates(false);
	USceneComponent* Root = NewObject<USceneComponent>(RenderHost, TEXT("SettlementTreeRoot"));
	RenderHost->AddInstanceComponent(Root);
	RenderHost->SetRootComponent(Root);
	Root->RegisterComponent();
	TreeMesh = FSettlementTreeMeshFactory::Create(*this);
	if (!TreeMesh)
	{
		ReleaseRenderHost();
		return false;
	}
	return true;
}

void USettlementTreePresentationWorldSubsystem::ReleaseRenderHost()
{
	TreeMesh = nullptr;
	if (IsValid(RenderHost))
	{
		RenderHost->Destroy();
	}
	RenderHost = nullptr;
}

void USettlementTreePresentationWorldSubsystem::ApplyPendingChanges(const double NowSeconds)
{
	if (!Data || !EnsureRenderHost())
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>();
	const double Start = FPlatformTime::Seconds();
	int32 RemainingBudget = Data->DynamicChangeBudget;
	// 原生 HISM
	// 调用不可中途抢占；限制单次批量尺寸，而同帧总吞吐仍由动态预算决定。
	const int32 MaxNativeBatchSize = FMath::Max(1, Settings->MaximumNativeInstanceBatchSize);
	bool bAppliedNativeBatch = false;
	const auto GetElapsedMilliseconds = [Start]() { return (FPlatformTime::Seconds() - Start) * 1000.0; };
	const auto IsTimeSliceExhausted = [&]()
	{
		// 原生 HISM 调用无法中途抢占；在 2ms 目标停止追加小批次，为 3.5ms
		// 硬片留余量。
		return bAppliedNativeBatch && GetElapsedMilliseconds() >= Settings->InstanceApplyTargetMilliseconds;
	};

	int32 GracePopCount = 0;
	while (RemainingBudget > 0 && !Data->GraceHeap.IsEmpty() && Data->GraceHeap.HeapTop().DeadlineSeconds <= NowSeconds)
	{
		if ((GracePopCount & 63) == 0 && GetElapsedMilliseconds() >= Settings->InstanceApplyTargetMilliseconds)
		{
			break;
		}
		FTreeDeadline Deadline;
		Data->GraceHeap.HeapPop(Deadline, FDeadlineMinHeap(), EAllowShrinking::No);
		++GracePopCount;
		--RemainingBudget;
		FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Deadline.Entity);
		if (!Slot || !Slot->bInGrace || Slot->GraceGeneration != Deadline.Generation || Slot->ReferenceCount != 0)
		{
			continue;
		}
		Data->CancelGrace(*Slot);
		Data->QueueRemove(*Slot);
	}

	while (RemainingBudget > 0 && Data->PendingRemoveHead < Data->PendingRemoves.Num() && !IsTimeSliceExhausted())
	{
		FIntPoint BatchCell = FIntPoint::ZeroValue;
		bool bHasBatchCell = false;
		TArray<int32> InstanceIndices;
		InstanceIndices.Reserve(FMath::Min(MaxNativeBatchSize, RemainingBudget));
		while (RemainingBudget > 0 && InstanceIndices.Num() < MaxNativeBatchSize &&
			   Data->PendingRemoveHead < Data->PendingRemoves.Num())
		{
			const FWorldObjectEntityHandle Entity = Data->PendingRemoves[Data->PendingRemoveHead];
			FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Entity);
			if (!Slot || !Slot->bPendingRemove)
			{
				++Data->PendingRemoveHead;
				continue;
			}
			if (!Slot->bInstanced || Slot->ReferenceCount != 0)
			{
				++Data->PendingRemoveHead;
				Slot->bPendingRemove = false;
				Data->PendingRemoveCount = FMath::Max(0, Data->PendingRemoveCount - 1);
				continue;
			}
			Data->AuditCurrentViewRemoval(*Slot, *Settings);
			if (!bHasBatchCell)
			{
				BatchCell = Slot->Cell;
				bHasBatchCell = true;
			}
			else if (Slot->Cell != BatchCell)
			{
				break;
			}

			++Data->PendingRemoveHead;
			Slot->bPendingRemove = false;
			Data->PendingRemoveCount = FMath::Max(0, Data->PendingRemoveCount - 1);
			InstanceIndices.Add(Slot->InstanceIndex);
			--RemainingBudget;
		}
		if (InstanceIndices.IsEmpty())
		{
			continue;
		}

		FSettlementTreePresentationData::FCluster* Cluster = Data->Clusters.Find(BatchCell);
		if (!Cluster || !Cluster->Component)
		{
			continue;
		}
		InstanceIndices.Sort([](const int32 Left, const int32 Right) { return Left > Right; });
		Cluster->Component->BeginBulkEdit();
		const bool bRemoved = Cluster->Component->RemoveInstances(InstanceIndices, true);
		Cluster->Component->EndBulkEdit(NowSeconds, bRemoved);
		bAppliedNativeBatch = true;
		if (!bRemoved)
		{
			for (const int32 InstanceIndex : InstanceIndices)
			{
				if (Cluster->Owners.IsValidIndex(InstanceIndex))
				{
					if (FSettlementTreePresentationData::FRenderSlot* Slot =
							Data->FindSlot(Cluster->Owners[InstanceIndex]))
					{
						Data->QueueRemove(*Slot);
					}
				}
			}
			continue;
		}
		for (const int32 InstanceIndex : InstanceIndices)
		{
			if (!Cluster->Owners.IsValidIndex(InstanceIndex))
			{
				continue;
			}
			const FWorldObjectEntityHandle Removed = Cluster->Owners[InstanceIndex];
			const FWorldObjectEntityHandle Moved = Cluster->Owners.Last();
			if (InstanceIndex != Cluster->Owners.Num() - 1)
			{
				Cluster->Owners[InstanceIndex] = Moved;
				if (FSettlementTreePresentationData::FRenderSlot* MovedSlot = Data->FindSlot(Moved))
				{
					MovedSlot->InstanceIndex = InstanceIndex;
				}
			}
			Cluster->Owners.Pop(EAllowShrinking::No);
				if (FSettlementTreePresentationData::FRenderSlot* RemovedSlot = Data->FindSlot(Removed))
				{
					Data->ClearCustomDataUpdate(*RemovedSlot);
					RemovedSlot->bInstanced = false;
				RemovedSlot->InstanceIndex = INDEX_NONE;
				Data->ResidentInstanceCount = FMath::Max(0, Data->ResidentInstanceCount - 1);
				++Data->Stats.HISMRemoveCount;
					if (RemovedSlot->bCatalogRemoved && RemovedSlot->ReferenceCount == 0)
					{
						Data->DiscardSlot(Removed);
					}
			}
		}
		Data->MarkClusterEdited(BatchCell, *Cluster, NowSeconds, *Settings);
			if (Cluster->Owners.IsEmpty())
			{
				Data->PendingEmptyClusters.AddUnique(BatchCell);
			}
		}

		struct FTreeCustomDataUpdate
		{
			FWorldObjectEntityHandle Entity;
			int32 InstanceIndex = INDEX_NONE;
		};
		TMap<FIntPoint, TArray<FTreeCustomDataUpdate>> CustomDataBatches;
		while (RemainingBudget > 0
			&& Data->PendingCustomDataUpdateHead < Data->PendingCustomDataUpdates.Num()
			&& !IsTimeSliceExhausted())
		{
			const FWorldObjectEntityHandle Entity =
				Data->PendingCustomDataUpdates[Data->PendingCustomDataUpdateHead++];
			FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Entity);
			if (!Slot || !Slot->bCustomDataDirty)
			{
				continue;
			}
			Data->ClearCustomDataUpdate(*Slot);
			if (!Slot->bInstanced || Slot->bPendingRemove || Slot->InstanceIndex == INDEX_NONE)
			{
				continue;
			}
			CustomDataBatches.FindOrAdd(Slot->Cell).Add({Entity, Slot->InstanceIndex});
			--RemainingBudget;
		}

		for (TPair<FIntPoint, TArray<FTreeCustomDataUpdate>>& Pair : CustomDataBatches)
		{
			FSettlementTreePresentationData::FCluster* Cluster = Data->Clusters.Find(Pair.Key);
			if (!Cluster || !Cluster->Component)
			{
				for (const FTreeCustomDataUpdate& Update : Pair.Value)
				{
					if (FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Update.Entity))
					{
						Data->QueueCustomDataUpdate(*Slot);
					}
				}
				continue;
			}
			Pair.Value.Sort([](const FTreeCustomDataUpdate& Left, const FTreeCustomDataUpdate& Right)
			{
				return Left.InstanceIndex < Right.InstanceIndex;
			});
			bool bUpdatedCluster = false;
			int32 RunStart = 0;
			while (RunStart < Pair.Value.Num())
			{
				int32 RunEnd = RunStart;
				while (RunEnd + 1 < Pair.Value.Num()
					&& Pair.Value[RunEnd + 1].InstanceIndex == Pair.Value[RunEnd].InstanceIndex + 1)
				{
					++RunEnd;
				}
				TArray<float> CustomData;
				CustomData.Reserve((RunEnd - RunStart + 1) * SettlementTreeCustomDataFloatCount);
				for (int32 Index = RunStart; Index <= RunEnd; ++Index)
				{
					const FSettlementTreePresentationData::FRenderSlot* Slot =
						Data->FindSlot(Pair.Value[Index].Entity);
					check(Slot && Slot->bInstanced && Slot->Cell == Pair.Key
						&& Slot->InstanceIndex == Pair.Value[Index].InstanceIndex);
					CustomData.Add(Slot->Candidate.ColorVariation);
					CustomData.Add(Slot->Candidate.BurnAmount);
				}
				const bool bUpdated = Cluster->Component->SetCustomDataRange(
					Pair.Value[RunStart].InstanceIndex,
					Pair.Value[RunEnd].InstanceIndex,
					CustomData);
				if (bUpdated)
				{
					Data->Stats.HISMCustomDataUpdateCount += RunEnd - RunStart + 1;
					bUpdatedCluster = true;
				}
				else
				{
					for (int32 Index = RunStart; Index <= RunEnd; ++Index)
					{
						if (FSettlementTreePresentationData::FRenderSlot* Slot =
							Data->FindSlot(Pair.Value[Index].Entity))
						{
							Data->QueueCustomDataUpdate(*Slot);
						}
					}
				}
				RunStart = RunEnd + 1;
			}
			if (bUpdatedCluster)
			{
				// SetCustomData 已通过 PrimitiveInstanceDataManager 合并为一次
				// MarkRenderInstancesDirty；不能再重建整个 Scene Proxy。全量重建会与
				// Renderer 的并行 MeshDrawCommand 缓存任务交叠，且完全没有必要。
				bAppliedNativeBatch = true;
			}
		}

		struct FTreeAddBatch
	{
		TArray<FSettlementTreeCandidate> Trees;
	};
	TMap<FIntPoint, FTreeAddBatch> AddBatches;
	int32 ScheduledAdds = 0;
	const int32 NormalLimit =
		FMath::Min(Settings->AbsoluteHardMax, Settings->StableInstanceBudget + Settings->TransitionReserve);
	const int32 EmergencyLimit =
		FMath::Min(Settings->AbsoluteHardMax,
				   Settings->StableInstanceBudget + Settings->TransitionReserve + Settings->EmergencyReserve);

	const auto DrainAddQueue = [this, &AddBatches, &Catalog, &RemainingBudget, &ScheduledAdds, MaxNativeBatchSize,
								&IsTimeSliceExhausted, &GetElapsedMilliseconds,
								Settings](TArray<FWorldObjectEntityHandle>& Queue, int32& Head,
										  const ETreeQueuedAddPriority RequiredPriority, const int32 AdmissionLimit)
	{
		while (RemainingBudget > 0 && Head < Queue.Num() && ScheduledAdds < MaxNativeBatchSize &&
			   Data->ResidentInstanceCount + ScheduledAdds < AdmissionLimit && !IsTimeSliceExhausted())
		{
			if (GetElapsedMilliseconds() >= Settings->InstanceApplyTargetMilliseconds)
			{
				break;
			}
			const FWorldObjectEntityHandle Entity = Queue[Head++];
			FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Entity);
			if (!Slot || Slot->QueuedAddPriority != RequiredPriority)
			{
				continue;
			}
			if (Slot->ReferenceCount == 0 || Slot->bInstanced)
			{
				Data->ClearPendingAdd(*Slot);
				continue;
			}
			FSettlementTreeCandidate Current;
			if (!Catalog || !Catalog->TryGetTree(Entity, Current))
			{
				Slot->bCatalogRemoved = true;
				Data->ClearPendingAdd(*Slot);
				continue;
			}
				Data->UpdateCandidate(*Slot, Current);
				AddBatches.FindOrAdd(Current.Cell).Trees.Add(Current);
			++ScheduledAdds;
			--RemainingBudget;
		}
	};

	DrainAddQueue(Data->EmergencyAdds, Data->EmergencyAddHead, ETreeQueuedAddPriority::Emergency, EmergencyLimit);
	DrainAddQueue(Data->NormalAdds, Data->NormalAddHead, ETreeQueuedAddPriority::Normal, NormalLimit);

	const auto RequeueUncommittedAddBatch = [this](const FTreeAddBatch& Batch)
	{
		for (const FSettlementTreeCandidate& Tree : Batch.Trees)
		{
			const FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Tree.Entity);
			if (!Slot)
			{
				continue;
			}
			if (Slot->QueuedAddPriority == ETreeQueuedAddPriority::Emergency)
			{
				Data->EmergencyAdds.Add(Tree.Entity);
			}
			else if (Slot->QueuedAddPriority == ETreeQueuedAddPriority::Normal)
			{
				Data->NormalAdds.Add(Tree.Entity);
			}
		}
	};

	for (TPair<FIntPoint, FTreeAddBatch>& Pair : AddBatches)
	{
		// 排队阶段很便宜，真正昂贵的是下面不可抢占的原生 HISM 提交。前一批
		// 已耗尽目标片后，把尚未提交的 Cell 原样放回队列，绝不在同帧硬冲完。
		if (IsTimeSliceExhausted())
		{
			RequeueUncommittedAddBatch(Pair.Value);
			continue;
		}
		FSettlementTreePresentationData::FCluster& Cluster = Data->Clusters.FindOrAdd(Pair.Key);
		if (!Cluster.Component)
		{
			Cluster.Component = NewObject<UDeferredHISMComponent>(
				RenderHost, MakeUniqueObjectName(RenderHost, UDeferredHISMComponent::StaticClass(),
												 TEXT("SettlementTreeCell")));
			Cluster.Component->SetupAttachment(RenderHost->GetRootComponent());
			Cluster.Component->SetStaticMesh(TreeMesh);
				Cluster.Component->SetNumCustomDataFloats(SettlementTreeCustomDataFloatCount);
			Cluster.Component->SetRemoveSwap();
			Cluster.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Cluster.Component->SetGenerateOverlapEvents(false);
			Cluster.Component->SetCanEverAffectNavigation(false);
			Cluster.Component->SetCastShadow(false);
			Cluster.Component->bCastDynamicShadow = false;
			Cluster.Component->bCastStaticShadow = false;
			Cluster.Component->SetReceivesDecals(false);
			RenderHost->AddInstanceComponent(Cluster.Component);
			Cluster.Component->RegisterComponent();
		}

		TArray<FTransform> Transforms;
		TArray<float> CustomData;
		Transforms.Reserve(Pair.Value.Trees.Num());
			CustomData.Reserve(Pair.Value.Trees.Num() * SettlementTreeCustomDataFloatCount);
		for (const FSettlementTreeCandidate& Tree : Pair.Value.Trees)
		{
				Transforms.Add(Tree.WorldTransform);
				CustomData.Add(Tree.ColorVariation);
				CustomData.Add(Tree.BurnAmount);
		}
		Cluster.Component->BeginBulkEdit();
		Cluster.Component->PreAllocateInstancesMemory(Transforms.Num());
		const TArray<int32> AddedIndices = Cluster.Component->AddInstances(Transforms, true, true, false);
		bool bComplete = AddedIndices.Num() == Pair.Value.Trees.Num();
		if (bComplete && !AddedIndices.IsEmpty())
		{
			const int32 FirstIndex = AddedIndices[0];
			for (int32 Index = 1; Index < AddedIndices.Num(); ++Index)
			{
				bComplete &= AddedIndices[Index] == FirstIndex + Index;
			}
			if (bComplete)
			{
				// 一次 memcpy + 一次连续范围通知，替代每棵树一次 SetCustomDataValue。
				bComplete = Cluster.Component->SetCustomDataRange(FirstIndex, AddedIndices.Last(), CustomData);
			}
		}
		if (bComplete)
		{
			for (int32 Index = 0; Index < AddedIndices.Num(); ++Index)
			{
				const FSettlementTreeCandidate& Tree = Pair.Value.Trees[Index];
				FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Tree.Entity);
				if (!Slot || Slot->ReferenceCount == 0)
				{
					continue;
				}
				const int32 InstanceIndex = AddedIndices[Index];
				check(InstanceIndex == Cluster.Owners.Num());
				Cluster.Owners.Add(Tree.Entity);
				Slot->Cell = Pair.Key;
					Slot->InstanceIndex = InstanceIndex;
					Slot->bInstanced = true;
					Data->ClearCustomDataUpdate(*Slot);
				Data->ClearPendingAdd(*Slot);
				++Data->ResidentInstanceCount;
				++Data->Stats.HISMAddCount;
				Data->OnInstanceAdded(Tree.Entity, NowSeconds);
			}
		}
		else
		{
			if (!AddedIndices.IsEmpty())
			{
				TArray<int32> Rollback = AddedIndices;
				Rollback.Sort([](const int32 Left, const int32 Right) { return Left > Right; });
				Cluster.Component->RemoveInstances(Rollback, true);
			}
			for (const FSettlementTreeCandidate& Tree : Pair.Value.Trees)
			{
				if (FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Tree.Entity))
				{
					if (Slot->QueuedAddPriority == ETreeQueuedAddPriority::Emergency)
					{
						Data->EmergencyAdds.Add(Tree.Entity);
					}
					else if (Slot->QueuedAddPriority == ETreeQueuedAddPriority::Normal)
					{
						Data->NormalAdds.Add(Tree.Entity);
					}
				}
			}
		}
		Cluster.Component->EndBulkEdit(NowSeconds, !AddedIndices.IsEmpty());
		bAppliedNativeBatch = true;
		if (!AddedIndices.IsEmpty())
		{
			// HISM 自己会在异步树应用时重建 Scene Proxy；此处只登记一次延迟层级树构建，
			// 不能额外 MarkRenderStateDirty，否则每个小批次都会再重建一次整个 Cell。
			Data->MarkClusterEdited(Pair.Key, Cluster, NowSeconds, *Settings);
		}
	}

	if (Data->EmergencyAddHead >= Data->EmergencyAdds.Num())
	{
		Data->EmergencyAdds.Reset();
		Data->EmergencyAddHead = 0;
	}
	if (Data->NormalAddHead >= Data->NormalAdds.Num())
	{
		Data->NormalAdds.Reset();
		Data->NormalAddHead = 0;
	}
		if (Data->PendingRemoveHead >= Data->PendingRemoves.Num())
	{
		Data->PendingRemoves.Reset();
		Data->PendingRemoveHead = 0;
	}

	while (RemainingBudget > 0 && !Data->PendingEmptyClusters.IsEmpty() && !IsTimeSliceExhausted())
	{
		const FIntPoint Cell = Data->PendingEmptyClusters.Pop(EAllowShrinking::No);
		--RemainingBudget;
		FSettlementTreePresentationData::FCluster* Cluster = Data->Clusters.Find(Cell);
		if (!Cluster || !Cluster->Owners.IsEmpty())
		{
			continue;
		}
		if (Data->PendingCustomDataUpdateHead >= Data->PendingCustomDataUpdates.Num())
		{
			Data->PendingCustomDataUpdates.Reset();
			Data->PendingCustomDataUpdateHead = 0;
		}
		if (Cluster->Component)
		{
			if (Cluster->Component->IsAsyncBuilding())
			{
				// UE 的异步 Builder 虽持有 WeakObjectPtr，但销毁正在建树的组件会让
				// Scene Proxy 生命周期与后台绘制命令缓存交叠。等构建回到 GameThread
				// 完成后再销毁；本帧停止，避免立刻重复 Pop 同一个 Cell。
				Data->PendingEmptyClusters.AddUnique(Cell);
				break;
			}
			Cluster->Component->DestroyComponent();
			bAppliedNativeBatch = true;
		}
		Data->Clusters.Remove(Cell);
	}

	const double ApplyMilliseconds = GetElapsedMilliseconds();
	Data->Stats.LastApplyMilliseconds = ApplyMilliseconds;
	if (ApplyMilliseconds > Settings->InstanceApplyTargetMilliseconds)
	{
		Data->DynamicChangeBudget =
			FMath::Max(Settings->MinimumChangesPerCycle, FMath::FloorToInt(Data->DynamicChangeBudget * 0.8));
	}
	else
	{
		Data->DynamicChangeBudget =
			FMath::Min(Settings->MaximumChangesPerCycle, FMath::CeilToInt(Data->DynamicChangeBudget * 1.1));
	}
	CSV_CUSTOM_STAT(SettlementTrees, InstanceApplyMilliseconds, ApplyMilliseconds, ECsvCustomStatOp::Set);
}

void USettlementTreePresentationWorldSubsystem::ProcessDeferredTreeBuilds(const double NowSeconds)
{
	if (!Data)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	if (Data->InFlightBuildCell.IsSet())
	{
		FSettlementTreePresentationData::FCluster* InFlightCluster =
			Data->Clusters.Find(Data->InFlightBuildCell.GetValue());
			if (InFlightCluster && InFlightCluster->Component && InFlightCluster->Component->IsAsyncBuilding())
		{
			Data->Stats.InFlightTreeBuildCount = 1;
			Data->Stats.LastTreeBuildScheduleMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
			CSV_CUSTOM_STAT(SettlementTrees, TreeBuildScheduleMilliseconds,
								Data->Stats.LastTreeBuildScheduleMilliseconds, ECsvCustomStatOp::Set);
				return;
			}
			if (InFlightCluster && InFlightCluster->Component)
			{
				InFlightCluster->Component->NotifyAsyncBuildObservedComplete();
				if (!InFlightCluster->Owners.IsEmpty() && InFlightCluster->Component->IsTreeFullyBuilt())
				{
					InFlightCluster->bHasBuiltPopulatedTree = true;
				}
			}
			Data->InFlightBuildCell.Reset();
		Data->Stats.InFlightTreeBuildCount = 0;
	}
	while (!Data->BuildHeap.IsEmpty())
	{
		const FTreeBuildDeadline Top = Data->BuildHeap.HeapTop();
		if (Top.DeadlineSeconds > NowSeconds)
		{
			break;
		}
		FTreeBuildDeadline Deadline;
		Data->BuildHeap.HeapPop(Deadline, FDeadlineMinHeap(), EAllowShrinking::No);
		FSettlementTreePresentationData::FCluster* Cluster = Data->Clusters.Find(Deadline.Cell);
		if (!Cluster || !Cluster->Component || Cluster->BuildGeneration != Deadline.Generation ||
			Cluster->Owners.IsEmpty())
		{
			continue;
		}
		const bool bStarted = Cluster->Component->TryStartDeferredTreeBuild(NowSeconds, Settings->TreeBuildQuietSeconds,
																			Settings->TreeBuildMaxDeferralSeconds);
		const uint64 BuildCount = Cluster->Component->GetTreeBuildCount();
		if (BuildCount >= Cluster->ObservedBuildCount)
		{
			Data->Stats.TreeBuildCount += static_cast<int64>(BuildCount - Cluster->ObservedBuildCount);
		}
		Cluster->ObservedBuildCount = BuildCount;
		if (bStarted)
		{
			Cluster->FirstDirtySeconds = 0.0;
			Cluster->LastDirtySeconds = 0.0;
			if (Cluster->Component->IsAsyncBuilding())
			{
				// Async HISM 会在 GameThread 应用整个重排结果。多个 Cell 同时完成会把
				// 数十个回调叠进同一帧，因此树模块全局只允许一个构建在途。
				Data->InFlightBuildCell = Deadline.Cell;
				Data->Stats.InFlightTreeBuildCount = 1;
				Data->Stats.MaximumConcurrentTreeBuildsObserved =
					FMath::Max(Data->Stats.MaximumConcurrentTreeBuildsObserved, 1);
			}
			else if (!Cluster->Owners.IsEmpty() && Cluster->Component->IsTreeFullyBuilt())
			{
				Cluster->bHasBuiltPopulatedTree = true;
			}
		}
		// 同步完成或成功启动一个异步构建后，本帧都不再启动第二个。
		break;
	}
	Data->Stats.LastTreeBuildScheduleMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	CSV_CUSTOM_STAT(SettlementTrees, TreeBuildScheduleMilliseconds, Data->Stats.LastTreeBuildScheduleMilliseconds,
						ECsvCustomStatOp::Set);
}

FSettlementTreePresentationStats USettlementTreePresentationWorldSubsystem::GetStats() const
{
	FSettlementTreePresentationStats Stats;
	if (!Data)
	{
		return Stats;
	}
	Stats = Data->Stats;
	Stats.ActiveCount = Data->ActiveLogicalCount;
	Stats.TransitionCount = Data->TransitionLogicalCount;
	Stats.GraceCount = Data->GraceCount;
	Stats.PendingCount = Data->PendingAddCount + Data->PendingRemoveCount
		+ Data->PendingCustomDataUpdateCount;
	Stats.HISMCellCount = Data->Clusters.Num();
	Stats.InstanceCount = Data->ResidentInstanceCount;
	Stats.RenderHostCount = IsValid(RenderHost) ? 1 : 0;
	return Stats;
}

bool USettlementTreePresentationWorldSubsystem::IsIdle() const
{
	return Data && !Data->HasPendingWork(FPlatformTime::Seconds());
}

bool USettlementTreePresentationWorldSubsystem::HasWorldEntityProjection(
	const FWorldEntityId WorldEntityId) const
{
	if (!Data || !WorldEntityId.IsSet())
	{
		return false;
	}
	const FWorldObjectEntityHandle* Entity = Data->EntityByWorldEntityId.Find(WorldEntityId);
	const FSettlementTreePresentationData::FRenderSlot* Slot = Entity
		? Data->FindSlot(*Entity) : nullptr;
	return Slot && Slot->bInstanced;
}

#if WITH_DEV_AUTOMATION_TESTS
bool USettlementTreePresentationWorldSubsystem::ValidateRenderMappingsForAutomation(FString& OutError) const
{
	OutError.Reset();
	if (!Data)
	{
		return true;
	}
	for (const TPair<FIntPoint, FSettlementTreePresentationData::FCluster>& Pair : Data->Clusters)
	{
		const FSettlementTreePresentationData::FCluster& Cluster = Pair.Value;
		if (!Cluster.Component)
		{
			OutError = FString::Printf(TEXT("Tree Cell (%d,%d) 没有 HISM Component。"), Pair.Key.X, Pair.Key.Y);
			return false;
		}
		if (Cluster.Component->GetInstanceCount() != Cluster.Owners.Num())
		{
			OutError = FString::Printf(TEXT("Tree Cell (%d,%d) HISM=%d Owner=%d 数量不一致。"), Pair.Key.X, Pair.Key.Y,
									   Cluster.Component->GetInstanceCount(), Cluster.Owners.Num());
			return false;
		}
		for (int32 InstanceIndex = 0; InstanceIndex < Cluster.Owners.Num(); ++InstanceIndex)
		{
			const FWorldObjectEntityHandle Owner = Cluster.Owners[InstanceIndex];
			const FSettlementTreePresentationData::FRenderSlot* Slot = Data->FindSlot(Owner);
			if (!Slot || !Slot->bInstanced || Slot->Cell != Pair.Key || Slot->InstanceIndex != InstanceIndex)
			{
				OutError = FString::Printf(TEXT("Tree Cell (%d,%d) Instance %d 的反向 Owner 映射失配。"), Pair.Key.X,
										   Pair.Key.Y, InstanceIndex);
				return false;
			}
			FTransform ActualTransform;
			if (!Cluster.Component->GetInstanceTransform(InstanceIndex, ActualTransform, true) ||
				!ActualTransform.Equals(Slot->Candidate.WorldTransform, 0.1))
			{
				OutError = FString::Printf(TEXT("Tree Cell (%d,%d) Instance %d 的 "
												"Transform 与 Owner 候选不一致。"),
										   Pair.Key.X, Pair.Key.Y, InstanceIndex);
				return false;
			}
		}
	}
	const auto ValidateForwardMapping = [&OutError, this](
		const FSettlementTreePresentationData::FRenderSlot& Slot)
	{
		if (!Slot.bInstanced)
		{
			return true;
		}
		const FSettlementTreePresentationData::FCluster* Cluster = Data->Clusters.Find(Slot.Cell);
		if (!Cluster || !Cluster->Owners.IsValidIndex(Slot.InstanceIndex) ||
			Cluster->Owners[Slot.InstanceIndex] != Slot.Entity)
		{
			OutError = FString::Printf(
				TEXT("Tree Slot %d Generation %u 的正向 Cell/Instance 映射失配。"),
				Slot.Entity.GetSlot(), Slot.Entity.GetGeneration());
			return false;
		}
		return true;
	};
	for (const FSettlementTreePresentationData::FRenderSlot& Slot : Data->Slots)
	{
		if (!ValidateForwardMapping(Slot))
		{
			return false;
		}
	}
	for (const TPair<FWorldObjectEntityHandle, FSettlementTreePresentationData::FRenderSlot>& Pair :
		 Data->RetiredSlots)
	{
		if (Pair.Key != Pair.Value.Entity)
		{
			OutError = TEXT("Tree Retired Slot 的完整 Handle Key 与记录不一致。");
			return false;
		}
		if (!ValidateForwardMapping(Pair.Value))
		{
			return false;
		}
	}
	return true;
}
#endif
