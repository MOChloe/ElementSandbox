#include "Tree/SettlementTreeCollisionWorldSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Subsystems/SubsystemCollection.h"
#include "Tree/SettlementTreeSettings.h"
#include "Tree/SettlementTreeWorldSubsystem.h"

namespace
{
	enum class ETreeCollisionAddPriority : uint8
	{
		None,
		Regular,
		Immediate
	};

	struct FCollisionGraceDeadline final
	{
		double DeadlineSeconds = 0.0;
		FWorldObjectEntityHandle Entity;
		uint32 Generation = 0;
	};

	struct FCollisionDeadlineMinHeap final
	{
		bool operator()(const FCollisionGraceDeadline& Left, const FCollisionGraceDeadline& Right) const
		{
			// UE 的 HeapPush/HeapPop 使用最小堆语义，较早到期项必须排在堆顶。
			return Left.DeadlineSeconds < Right.DeadlineSeconds;
		}
	};

	bool AreSourcesEquivalentIgnoringRevision(
		const FSettlementTreeCollisionSource& Left,
		const FSettlementTreeCollisionSource& Right)
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
			&& BoxesEqual(Left.ImmediateBounds, Right.ImmediateBounds)
			&& BoxesEqual(Left.PrefetchBounds, Right.PrefetchBounds)
			&& BoxesEqual(Left.RetentionBounds, Right.RetentionBounds);
	}
}

class FSettlementTreeCollisionData final
{
public:
	struct FSourceSlot final
	{
		FSettlementTreeCollisionSource Source;
		TSet<FWorldObjectEntityHandle> Immediate;
		TSet<FWorldObjectEntityHandle> Prefetch;
		TSet<FWorldObjectEntityHandle> Retention;
		TSet<FIntPoint> QueriedCells;
		uint32 Generation = 1;
		int32 NextFree = INDEX_NONE;
		bool bAlive = false;
		bool bDirty = false;
	};

	struct FCollisionSlot final
	{
		FWorldObjectEntityHandle Entity;
		FSettlementTreeCandidate Candidate;
		int32 InstanceIndex = INDEX_NONE;
		int32 ImmediateReferences = 0;
		int32 PrefetchReferences = 0;
		int32 RetentionReferences = 0;
		uint32 GraceGeneration = 0;
		ETreeCollisionAddPriority QueuedAddPriority = ETreeCollisionAddPriority::None;
		bool bActive = false;
		bool bPendingRemove = false;
		bool bInGrace = false;
	};

	void EnsureCollisionSlot(const FSettlementTreeCandidate& Candidate)
	{
		const int32 SlotIndex = Candidate.Entity.GetSlot();
		if (SlotIndex >= 0 && CollisionSlots.Num() <= SlotIndex)
		{
			CollisionSlots.SetNum(SlotIndex + 1);
		}
		FCollisionSlot& Slot = CollisionSlots[SlotIndex];
		if (Slot.Entity.IsSet() && Slot.Entity != Candidate.Entity)
		{
			const FWorldObjectEntityHandle RetiredEntity = Slot.Entity;
			check(!RetiredCollisionSlots.Contains(RetiredEntity));
			// Collision Remove 同样受帧预算与 Grace 约束；旧 Generation 必须保留到
			// 物理实例和所有 Source 引用真正退场。
			RetiredCollisionSlots.Add(RetiredEntity, MoveTemp(Slot));
			Slot = {};
		}
		Slot.Entity = Candidate.Entity;
		Slot.Candidate = Candidate;
	}

	FCollisionSlot* FindCollisionSlot(const FWorldObjectEntityHandle Entity)
	{
		if (CollisionSlots.IsValidIndex(Entity.GetSlot())
			&& CollisionSlots[Entity.GetSlot()].Entity == Entity)
		{
			return &CollisionSlots[Entity.GetSlot()];
		}
		return RetiredCollisionSlots.Find(Entity);
	}

	void ClearPendingAdd(FCollisionSlot& Slot)
	{
		if (Slot.QueuedAddPriority != ETreeCollisionAddPriority::None)
		{
			Slot.QueuedAddPriority = ETreeCollisionAddPriority::None;
			PendingAddCount = FMath::Max(0, PendingAddCount - 1);
		}
	}

	void QueueAdd(FCollisionSlot& Slot, const ETreeCollisionAddPriority Priority)
	{
		if (Slot.bActive || Priority == ETreeCollisionAddPriority::None)
		{
			return;
		}
		if (Slot.QueuedAddPriority == ETreeCollisionAddPriority::None)
		{
			Slot.QueuedAddPriority = Priority;
			++PendingAddCount;
			if (Priority == ETreeCollisionAddPriority::Immediate)
			{
				PendingImmediateAdds.Add(Slot.Entity);
			}
			else
			{
				PendingAdds.Add(Slot.Entity);
			}
		}
		else if (Slot.QueuedAddPriority == ETreeCollisionAddPriority::Regular
			&& Priority == ETreeCollisionAddPriority::Immediate)
		{
			Slot.QueuedAddPriority = ETreeCollisionAddPriority::Immediate;
			PendingImmediateAdds.Add(Slot.Entity);
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

	void DiscardCollisionSlot(const FWorldObjectEntityHandle Entity)
	{
		FCollisionSlot* Slot = FindCollisionSlot(Entity);
		if (!Slot)
		{
			return;
		}
		check(!Slot->bActive && Slot->ImmediateReferences == 0
			&& Slot->PrefetchReferences == 0 && Slot->RetentionReferences == 0);
		ClearPendingAdd(*Slot);
		CancelGrace(*Slot);
		CancelRemove(*Slot);
		if (CollisionSlots.IsValidIndex(Entity.GetSlot())
			&& CollisionSlots[Entity.GetSlot()].Entity == Entity)
		{
			CollisionSlots[Entity.GetSlot()] = {};
		}
		else
		{
			RetiredCollisionSlots.Remove(Entity);
		}
	}

	void QueueRemove(FCollisionSlot& Slot)
	{
		if (!Slot.bActive || Slot.bPendingRemove)
		{
			return;
		}
		Slot.bPendingRemove = true;
		++PendingRemoveCount;
		PendingRemoves.Add(Slot.Entity);
	}

	void ReconcileSlot(
		FCollisionSlot& Slot,
		const double NowSeconds,
		const USettlementTreeSettings& Settings)
	{
		if (Slot.RetentionReferences > 0)
		{
			CancelGrace(Slot);
			CancelRemove(Slot);
			if (!Slot.bActive)
			{
				if (Slot.ImmediateReferences > 0)
				{
					QueueAdd(Slot, ETreeCollisionAddPriority::Immediate);
				}
				else if (Slot.PrefetchReferences > 0)
				{
					QueueAdd(Slot, ETreeCollisionAddPriority::Regular);
				}
				else
				{
					ClearPendingAdd(Slot);
				}
			}
			return;
		}

		ClearPendingAdd(Slot);
		if (!Slot.bActive)
		{
			if (Slot.ImmediateReferences == 0 && Slot.PrefetchReferences == 0)
			{
				DiscardCollisionSlot(Slot.Entity);
			}
			return;
		}
		if (!Slot.bInGrace)
		{
			Slot.bInGrace = true;
			const uint32 Generation = ++Slot.GraceGeneration;
			GraceHeap.HeapPush(
				FCollisionGraceDeadline{
					NowSeconds + Settings.CollisionGraceSeconds,
					Slot.Entity,
					Generation},
				FCollisionDeadlineMinHeap());
		}
	}

	void RemoveSourceMembership(
		FSourceSlot& Source,
		TSet<FWorldObjectEntityHandle>& Touched)
	{
		for (const FWorldObjectEntityHandle Entity : Source.Immediate)
		{
			if (FCollisionSlot* Slot = FindCollisionSlot(Entity))
			{
				check(Slot->ImmediateReferences > 0);
				--Slot->ImmediateReferences;
				Touched.Add(Entity);
			}
		}
		for (const FWorldObjectEntityHandle Entity : Source.Prefetch)
		{
			if (FCollisionSlot* Slot = FindCollisionSlot(Entity))
			{
				check(Slot->PrefetchReferences > 0);
				--Slot->PrefetchReferences;
				Touched.Add(Entity);
			}
		}
		for (const FWorldObjectEntityHandle Entity : Source.Retention)
		{
			if (FCollisionSlot* Slot = FindCollisionSlot(Entity))
			{
				check(Slot->RetentionReferences > 0);
				--Slot->RetentionReferences;
				Touched.Add(Entity);
			}
		}
		Source.Immediate.Reset();
		Source.Prefetch.Reset();
		Source.Retention.Reset();
		Source.QueriedCells.Reset();
	}

	bool HasPendingWork() const
	{
		if (DirtySourceCount > 0 || PendingAddCount > 0 || PendingRemoveCount > 0 || !GraceHeap.IsEmpty())
		{
			return true;
		}
		return false;
	}

	TArray<FSourceSlot> Sources;
	int32 FirstFreeSource = INDEX_NONE;
	int32 SourceCount = 0;
	int32 DirtySourceCount = 0;
	TArray<FCollisionSlot> CollisionSlots;
	TMap<FWorldObjectEntityHandle, FCollisionSlot> RetiredCollisionSlots;
	TArray<FWorldObjectEntityHandle> Owners;
	TArray<FWorldObjectEntityHandle> PendingAdds;
	TArray<FWorldObjectEntityHandle> PendingImmediateAdds;
	TArray<FWorldObjectEntityHandle> PendingRemoves;
	TArray<FCollisionGraceDeadline> GraceHeap;
	int32 PendingAddHead = 0;
	int32 PendingImmediateAddHead = 0;
	int32 PendingRemoveHead = 0;
	int32 PendingAddCount = 0;
	int32 PendingRemoveCount = 0;
	FSettlementTreeCollisionStats Stats;
};

USettlementTreeCollisionWorldSubsystem::USettlementTreeCollisionWorldSubsystem() = default;
USettlementTreeCollisionWorldSubsystem::~USettlementTreeCollisionWorldSubsystem() = default;

void USettlementTreeCollisionWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USettlementTreeWorldSubsystem>();
	Data = MakePimpl<FSettlementTreeCollisionData>();
	if (USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		CellsPublishedHandle = Catalog->OnCellsPublished().AddUObject(
			this,
			&USettlementTreeCollisionWorldSubsystem::HandleCellsPublished);
	}
}

void USettlementTreeCollisionWorldSubsystem::Deinitialize()
{
	if (USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		Catalog->OnCellsPublished().Remove(CellsPublishedHandle);
	}
	ReleaseCollisionHost();
	Data.Reset();
	Super::Deinitialize();
}

bool USettlementTreeCollisionWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool USettlementTreeCollisionWorldSubsystem::IsTickable() const
{
	return Data && Data->HasPendingWork();
}

TStatId USettlementTreeCollisionWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USettlementTreeCollisionWorldSubsystem, STATGROUP_Tickables);
}

FSettlementTreeCollisionSourceHandle USettlementTreeCollisionWorldSubsystem::RegisterSource(
	const FSettlementTreeCollisionSource& Source)
{
	if (!Data || !Source.IsValid())
	{
		return {};
	}
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
	FSettlementTreeCollisionData::FSourceSlot& Entry = Data->Sources[SlotIndex];
	Entry.Source = Source;
	Entry.NextFree = INDEX_NONE;
	Entry.bAlive = true;
	Entry.bDirty = true;
	++Data->SourceCount;
	++Data->DirtySourceCount;
	++Data->Stats.SourceSubmitCount;
	return {SlotIndex, Entry.Generation};
}

bool USettlementTreeCollisionWorldSubsystem::UpdateSource(
	const FSettlementTreeCollisionSourceHandle Handle,
	const FSettlementTreeCollisionSource& Source)
{
	if (!Data || !Source.IsValid() || !Data->Sources.IsValidIndex(Handle.Slot))
	{
		return false;
	}
	FSettlementTreeCollisionData::FSourceSlot& Entry = Data->Sources[Handle.Slot];
	if (!Entry.bAlive || Entry.Generation != Handle.Generation)
	{
		return false;
	}
	if (AreSourcesEquivalentIgnoringRevision(Entry.Source, Source))
	{
		return true;
	}
	Entry.Source = Source;
	if (!Entry.bDirty)
	{
		Entry.bDirty = true;
		++Data->DirtySourceCount;
	}
	++Data->Stats.SourceSubmitCount;
	return true;
}

bool USettlementTreeCollisionWorldSubsystem::UnregisterSource(
	const FSettlementTreeCollisionSourceHandle Handle)
{
	if (!Data || !Data->Sources.IsValidIndex(Handle.Slot))
	{
		return false;
	}
	FSettlementTreeCollisionData::FSourceSlot& Entry = Data->Sources[Handle.Slot];
	if (!Entry.bAlive || Entry.Generation != Handle.Generation)
	{
		return false;
	}
	TSet<FWorldObjectEntityHandle> Touched;
	Data->RemoveSourceMembership(Entry, Touched);
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FWorldObjectEntityHandle Entity : Touched)
	{
		if (FSettlementTreeCollisionData::FCollisionSlot* Slot = Data->FindCollisionSlot(Entity))
		{
			Data->ReconcileSlot(*Slot, NowSeconds, *Settings);
		}
	}
	if (Entry.bDirty)
	{
		Data->DirtySourceCount = FMath::Max(0, Data->DirtySourceCount - 1);
	}
	Entry = {};
	Entry.Generation = Handle.Generation == MAX_uint32 ? 1 : Handle.Generation + 1;
	Entry.NextFree = Data->FirstFreeSource;
	Data->FirstFreeSource = Handle.Slot;
	--Data->SourceCount;
	return true;
}

void USettlementTreeCollisionWorldSubsystem::HandleCellsPublished(
	const TConstArrayView<FSettlementTreeCellChange> Changes)
{
	if (!Data || Changes.IsEmpty())
	{
		return;
	}
	for (FSettlementTreeCollisionData::FSourceSlot& Source : Data->Sources)
	{
		if (!Source.bAlive || Source.bDirty)
		{
			continue;
		}
		bool bRelevant = false;
		for (const FSettlementTreeCellChange& Change : Changes)
		{
			if (Change.RemovedEntities)
			{
				for (const FWorldObjectEntityHandle Entity : *Change.RemovedEntities)
				{
					if (Source.Retention.Contains(Entity))
					{
						bRelevant = true;
						break;
					}
				}
			}
			if (!bRelevant && Change.UpsertedTrees)
			{
				for (const FSettlementTreeCandidate& Candidate : *Change.UpsertedTrees)
				{
					// 已在 Retention 中的树即使原地更新后移出 Bounds，也必须刷新；
					// 同 1km Cell 的远处新增树不再把近场 Source 反复标脏。
					if (Source.Retention.Contains(Candidate.Entity) ||
						Source.Source.RetentionBounds.Intersect(Candidate.WorldBounds))
					{
						bRelevant = true;
						break;
					}
				}
			}
			if (bRelevant)
			{
				break;
			}
		}
		if (bRelevant)
		{
			Source.bDirty = true;
			++Data->DirtySourceCount;
		}
	}
}

void USettlementTreeCollisionWorldSubsystem::RefreshDirtySources()
{
	USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>();
	if (!Data || !Catalog || Data->DirtySourceCount == 0)
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	const double NowSeconds = FPlatformTime::Seconds();
	TArray<FSettlementTreeCandidate> Candidates;
	for (FSettlementTreeCollisionData::FSourceSlot& Source : Data->Sources)
	{
		if (!Source.bAlive || !Source.bDirty)
		{
			continue;
		}
		TSet<FWorldObjectEntityHandle> Touched;
		Data->RemoveSourceMembership(Source, Touched);
		Catalog->QueryTrees(Source.Source.RetentionBounds, Candidates);
		++Data->Stats.CatalogQueryCount;
		for (const FSettlementTreeCandidate& Candidate : Candidates)
		{
			++Data->Stats.CandidateTestCount;
			Data->EnsureCollisionSlot(Candidate);
			FSettlementTreeCollisionData::FCollisionSlot& Slot =
				Data->CollisionSlots[Candidate.Entity.GetSlot()];
			Slot.Candidate = Candidate;
			Source.Retention.Add(Candidate.Entity);
			Source.QueriedCells.Add(Candidate.Cell);
			++Slot.RetentionReferences;
			Touched.Add(Candidate.Entity);
			if (Source.Source.PrefetchBounds.Intersect(Candidate.WorldBounds))
			{
				Source.Prefetch.Add(Candidate.Entity);
				++Slot.PrefetchReferences;
			}
			if (Source.Source.ImmediateBounds.Intersect(Candidate.WorldBounds))
			{
				Source.Immediate.Add(Candidate.Entity);
				++Slot.ImmediateReferences;
			}
		}
		for (const FWorldObjectEntityHandle Entity : Touched)
		{
			if (FSettlementTreeCollisionData::FCollisionSlot* Slot = Data->FindCollisionSlot(Entity))
			{
				Data->ReconcileSlot(*Slot, NowSeconds, *Settings);
			}
		}
		Source.bDirty = false;
		Data->DirtySourceCount = FMath::Max(0, Data->DirtySourceCount - 1);
	}
}

void USettlementTreeCollisionWorldSubsystem::Tick(const float DeltaTime)
{
	(void)DeltaTime;
	if (!Data)
	{
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	RefreshDirtySources();
	ExpireGrace(FPlatformTime::Seconds());
	ApplyAdds(MAX_int32, true);
	ApplyAdds(Settings->PredictiveAddsPerFrame, false);
	ApplyRemoves(Settings->RemovesPerFrame);
	if (Data->SourceCount == 0
		&& Data->Owners.IsEmpty()
		&& Data->PendingAddCount == 0
		&& Data->PendingRemoveCount == 0)
	{
		ReleaseCollisionHost();
	}
}

void USettlementTreeCollisionWorldSubsystem::FlushImmediateCollisionChanges()
{
	if (!Data || Data->SourceCount == 0)
	{
		return;
	}
	RefreshDirtySources();
	ExpireGrace(FPlatformTime::Seconds());
	ApplyAdds(MAX_int32, true);
}

void USettlementTreeCollisionWorldSubsystem::ExpireGrace(const double NowSeconds)
{
	if (!Data)
	{
		return;
	}
	while (!Data->GraceHeap.IsEmpty()
		&& Data->GraceHeap.HeapTop().DeadlineSeconds <= NowSeconds)
	{
		FCollisionGraceDeadline Deadline;
		Data->GraceHeap.HeapPop(
			Deadline,
			FCollisionDeadlineMinHeap(),
			EAllowShrinking::No);
		FSettlementTreeCollisionData::FCollisionSlot* Slot =
			Data->FindCollisionSlot(Deadline.Entity);
		if (!Slot
			|| !Slot->bInGrace
			|| Slot->GraceGeneration != Deadline.Generation
			|| Slot->RetentionReferences != 0)
		{
			continue;
		}
		Slot->bInGrace = false;
		Data->QueueRemove(*Slot);
	}
}

bool USettlementTreeCollisionWorldSubsystem::EnsureCollisionHost()
{
	if (IsValid(CollisionHost) && IsValid(CollisionInstances))
	{
		return true;
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CollisionHost = GetWorldRef().SpawnActor<AActor>(Parameters);
	if (!CollisionHost)
	{
		return false;
	}
	CollisionHost->SetReplicates(false);
	USceneComponent* Root = NewObject<USceneComponent>(
		CollisionHost,
		TEXT("SettlementTreeCollisionRoot"));
	CollisionHost->AddInstanceComponent(Root);
	CollisionHost->SetRootComponent(Root);
	Root->RegisterComponent();
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!Cylinder)
	{
		ReleaseCollisionHost();
		return false;
	}
	CollisionInstances = NewObject<UInstancedStaticMeshComponent>(
		CollisionHost,
		TEXT("SettlementTreeCollisionISM"));
	CollisionInstances->SetupAttachment(Root);
	CollisionInstances->SetStaticMesh(Cylinder);
	CollisionInstances->SetRemoveSwap();
	CollisionInstances->SetVisibility(false, true);
	CollisionInstances->SetHiddenInGame(true);
	CollisionInstances->SetCastShadow(false);
	CollisionInstances->SetCanEverAffectNavigation(false);
	CollisionInstances->SetGenerateOverlapEvents(false);
	CollisionInstances->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	CollisionInstances->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionHost->AddInstanceComponent(CollisionInstances);
	CollisionInstances->RegisterComponent();
	return true;
}

void USettlementTreeCollisionWorldSubsystem::ReleaseCollisionHost()
{
	CollisionInstances = nullptr;
	if (IsValid(CollisionHost))
	{
		CollisionHost->Destroy();
	}
	CollisionHost = nullptr;
}

void USettlementTreeCollisionWorldSubsystem::ApplyAdds(
	const int32 Budget,
	const bool bImmediateOnly)
{
	if (!Data || Budget <= 0)
	{
		return;
	}
	TArray<FWorldObjectEntityHandle>& Queue = bImmediateOnly
		? Data->PendingImmediateAdds
		: Data->PendingAdds;
	int32& Head = bImmediateOnly
		? Data->PendingImmediateAddHead
		: Data->PendingAddHead;
	const ETreeCollisionAddPriority RequiredPriority = bImmediateOnly
		? ETreeCollisionAddPriority::Immediate
		: ETreeCollisionAddPriority::Regular;
	if (Head >= Queue.Num())
	{
		Queue.Reset();
		Head = 0;
		return;
	}
	if (!EnsureCollisionHost())
	{
		return;
	}
	USettlementTreeWorldSubsystem* Catalog =
		GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>();
	int32 Applied = 0;
	while (Applied < Budget && Head < Queue.Num())
	{
		const FWorldObjectEntityHandle Entity = Queue[Head++];
		FSettlementTreeCollisionData::FCollisionSlot* Slot =
			Data->FindCollisionSlot(Entity);
		if (!Slot || Slot->QueuedAddPriority != RequiredPriority)
		{
			continue;
		}
		const bool bStillDesired = bImmediateOnly
			? Slot->ImmediateReferences > 0
			: Slot->PrefetchReferences > 0;
		if (Slot->bActive || !bStillDesired || Slot->RetentionReferences == 0)
		{
			Data->ClearPendingAdd(*Slot);
			continue;
		}
		FSettlementTreeCandidate Tree;
		if (!Catalog || !Catalog->TryGetTree(Entity, Tree))
		{
			Data->ClearPendingAdd(*Slot);
			continue;
		}
		// Engine Cylinder 原始半径/半高均为 50cm；整树 Transform 含稳定尺寸变化。
		const FTransform TrunkLocal(
			FQuat::Identity,
			FVector(0.0, 0.0, 130.0),
			FVector(0.3, 0.3, 2.6));
		const int32 InstanceIndex = CollisionInstances->AddInstance(
			TrunkLocal * Tree.WorldTransform,
			true);
		if (InstanceIndex == INDEX_NONE)
		{
			Queue.Add(Entity);
			continue;
		}
		check(InstanceIndex == Data->Owners.Num());
		Slot->Candidate = Tree;
		Slot->InstanceIndex = InstanceIndex;
		Slot->bActive = true;
		Data->ClearPendingAdd(*Slot);
		Data->Owners.Add(Entity);
		++Applied;
	}
	if (Head >= Queue.Num())
	{
		Queue.Reset();
		Head = 0;
	}
	if (Applied > 0)
	{
		CollisionInstances->MarkRenderStateDirty();
	}
}

void USettlementTreeCollisionWorldSubsystem::ApplyRemoves(const int32 Budget)
{
	if (!Data || !CollisionInstances || Budget <= 0)
	{
		return;
	}
	TArray<int32> Indices;
	while (Indices.Num() < Budget && Data->PendingRemoveHead < Data->PendingRemoves.Num())
	{
		const FWorldObjectEntityHandle Entity =
			Data->PendingRemoves[Data->PendingRemoveHead++];
		FSettlementTreeCollisionData::FCollisionSlot* Slot =
			Data->FindCollisionSlot(Entity);
		if (!Slot || !Slot->bPendingRemove)
		{
			continue;
		}
		Slot->bPendingRemove = false;
		Data->PendingRemoveCount = FMath::Max(0, Data->PendingRemoveCount - 1);
		if (Slot->bActive && Slot->RetentionReferences == 0 && !Slot->bInGrace)
		{
			Indices.Add(Slot->InstanceIndex);
		}
	}
	if (Data->PendingRemoveHead >= Data->PendingRemoves.Num())
	{
		Data->PendingRemoves.Reset();
		Data->PendingRemoveHead = 0;
	}
	Indices.Sort(TGreater<int32>());
	for (const int32 InstanceIndex : Indices)
	{
		if (!Data->Owners.IsValidIndex(InstanceIndex))
		{
			continue;
		}
		const FWorldObjectEntityHandle Removed = Data->Owners[InstanceIndex];
		const FWorldObjectEntityHandle Moved = Data->Owners.Last();
		if (InstanceIndex != Data->Owners.Num() - 1)
		{
			Data->Owners[InstanceIndex] = Moved;
			if (FSettlementTreeCollisionData::FCollisionSlot* MovedSlot =
				Data->FindCollisionSlot(Moved))
			{
				MovedSlot->InstanceIndex = InstanceIndex;
			}
		}
		Data->Owners.Pop(EAllowShrinking::No);
		if (FSettlementTreeCollisionData::FCollisionSlot* RemovedSlot =
			Data->FindCollisionSlot(Removed))
		{
			RemovedSlot->bActive = false;
			RemovedSlot->InstanceIndex = INDEX_NONE;
			if (RemovedSlot->RetentionReferences == 0
				&& RemovedSlot->QueuedAddPriority == ETreeCollisionAddPriority::None)
			{
				Data->DiscardCollisionSlot(Removed);
			}
		}
	}
	if (!Indices.IsEmpty())
	{
		CollisionInstances->RemoveInstances(Indices, true);
	}
}

FSettlementTreeCollisionStats USettlementTreeCollisionWorldSubsystem::GetStats() const
{
	FSettlementTreeCollisionStats Stats;
	if (!Data)
	{
		return Stats;
	}
	Stats = Data->Stats;
	Stats.SourceCount = Data->SourceCount;
	Stats.CollisionInstanceCount = Data->Owners.Num();
	Stats.PendingAddCount = Data->PendingAddCount;
	Stats.PendingRemoveCount = Data->PendingRemoveCount;
	return Stats;
}

bool USettlementTreeCollisionWorldSubsystem::IsIdle() const
{
	return Data && !Data->HasPendingWork();
}
