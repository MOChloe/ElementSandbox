#include "Tree/SettlementTreeWorldSubsystem.h"

#include "ElementSandboxWorldObjectCatalog.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Subsystems/SubsystemCollection.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Tree/SettlementTreeSettings.h"
#include "WorldObjectWorldSubsystem.h"

CSV_DECLARE_CATEGORY_EXTERN(SettlementTrees);

namespace
{
FIntPoint ResolveTreeCell(const FVector& Location, const double CellSize)
{
	return FIntPoint(FMath::FloorToInt(Location.X / CellSize), FMath::FloorToInt(Location.Y / CellSize));
}

constexpr double SnapshotShardSizeCentimeters = 10000.0;
} // namespace

class FSettlementTreeWorldData final
{
public:
	struct FSlot final
	{
		FWorldObjectEntityHandle Entity;
		FWorldEntityId WorldEntityId;
		FTransform WorldTransform = FTransform::Identity;
		FBox WorldBounds = FBox(ForceInit);
		FIntPoint Cell = FIntPoint::ZeroValue;
		int32 CellIndex = INDEX_NONE;
		FIntPoint SnapshotShard = FIntPoint::ZeroValue;
		int32 SnapshotShardIndex = INDEX_NONE;
		float BurnAmount = 0.0f;
		bool bAlive = false;
	};

	struct FSnapshotShardData final
	{
		TArray<FWorldObjectEntityHandle> Trees;
		TSharedPtr<const FSettlementTreeSnapshotShard, ESPMode::ThreadSafe> Snapshot;
	};

	struct FCell final
	{
		TArray<FWorldObjectEntityHandle> Trees;
		TMap<FIntPoint, FSnapshotShardData> SnapshotShards;
		TSharedPtr<const FSettlementTreeCellSnapshot, ESPMode::ThreadSafe> Snapshot;
	};

	struct FPendingCellChange final
	{
		TSet<FWorldObjectEntityHandle> UpsertedEntities;
		TSet<FWorldObjectEntityHandle> RemovedEntities;
		TSet<FIntPoint> DirtySnapshotShards;
	};

	void MarkUpserted(const FIntPoint Cell, const FIntPoint SnapshotShard, const FWorldObjectEntityHandle Entity)
	{
		FPendingCellChange& Pending = PendingCellChanges.FindOrAdd(Cell);
		Pending.RemovedEntities.Remove(Entity);
		Pending.UpsertedEntities.Add(Entity);
		Pending.DirtySnapshotShards.Add(SnapshotShard);
		DirtySnapshotCells.Add(Cell);
	}

	void MarkRemoved(const FIntPoint Cell, const FIntPoint SnapshotShard, const FWorldObjectEntityHandle Entity)
	{
		FPendingCellChange& Pending = PendingCellChanges.FindOrAdd(Cell);
		if (Pending.UpsertedEntities.Remove(Entity) == 0)
		{
			Pending.RemovedEntities.Add(Entity);
		}
		Pending.DirtySnapshotShards.Add(SnapshotShard);
		DirtySnapshotCells.Add(Cell);
	}

	void EnsureSlot(const int32 Slot)
	{
		if (Slot >= 0 && Slots.Num() <= Slot)
		{
			Slots.SetNum(Slot + 1);
		}
	}

	void Remove(const FWorldObjectEntityHandle Entity)
	{
		if (!Slots.IsValidIndex(Entity.GetSlot()))
		{
			return;
		}
		FSlot& Slot = Slots[Entity.GetSlot()];
		if (!Slot.bAlive || Slot.Entity != Entity)
		{
			return;
		}
		const FIntPoint RemovedCell = Slot.Cell;
		const FIntPoint RemovedSnapshotShard = Slot.SnapshotShard;
		FCell* Cell = Cells.Find(RemovedCell);
		if (Cell)
		{
			if (FSnapshotShardData* Shard = Cell->SnapshotShards.Find(RemovedSnapshotShard))
			{
				if (Shard->Trees.IsValidIndex(Slot.SnapshotShardIndex))
				{
					const int32 LastShardIndex = Shard->Trees.Num() - 1;
					if (Slot.SnapshotShardIndex != LastShardIndex)
					{
						const FWorldObjectEntityHandle Moved = Shard->Trees[LastShardIndex];
						Shard->Trees[Slot.SnapshotShardIndex] = Moved;
						if (Slots.IsValidIndex(Moved.GetSlot()) && Slots[Moved.GetSlot()].Entity == Moved)
						{
							Slots[Moved.GetSlot()].SnapshotShardIndex = Slot.SnapshotShardIndex;
						}
					}
					Shard->Trees.RemoveAt(LastShardIndex, EAllowShrinking::No);
				}
				if (Shard->Trees.IsEmpty())
				{
					Cell->SnapshotShards.Remove(RemovedSnapshotShard);
				}
			}

			if (Cell->Trees.IsValidIndex(Slot.CellIndex))
			{
				const int32 LastIndex = Cell->Trees.Num() - 1;
				if (Slot.CellIndex != LastIndex)
				{
					const FWorldObjectEntityHandle Moved = Cell->Trees[LastIndex];
					Cell->Trees[Slot.CellIndex] = Moved;
					if (Slots.IsValidIndex(Moved.GetSlot()) && Slots[Moved.GetSlot()].Entity == Moved)
					{
						Slots[Moved.GetSlot()].CellIndex = Slot.CellIndex;
					}
				}
				Cell->Trees.RemoveAt(LastIndex, EAllowShrinking::No);
			}
			if (Cell->Trees.IsEmpty())
			{
				Cells.Remove(RemovedCell);
			}
		}
		MarkRemoved(RemovedCell, RemovedSnapshotShard, Entity);
		Slot = {};
		--ResidentCount;
		++Revision;
	}

	void Upsert(const FWorldObjectLifecycleRecord& Record, const float BurnAmount)
	{
		EnsureSlot(Record.Entity.GetSlot());
		FSlot& Slot = Slots[Record.Entity.GetSlot()];
		if (Slot.bAlive)
		{
			if (Slot.Entity == Record.Entity && Slot.WorldTransform.Equals(Record.WorldTransform))
			{
				if (!FMath::IsNearlyEqual(Slot.BurnAmount, BurnAmount))
				{
					Slot.BurnAmount = BurnAmount;
					MarkUpserted(Slot.Cell, Slot.SnapshotShard, Record.Entity);
					++Revision;
				}
				return;
			}
			Remove(Slot.Entity);
		}
		const FIntPoint CellCoordinate = ResolveTreeCell(Record.WorldTransform.GetLocation(), CellSize);
		const FIntPoint SnapshotShardCoordinate =
			ResolveTreeCell(Record.WorldTransform.GetLocation(), SnapshotShardSizeCentimeters);
		FCell& Cell = Cells.FindOrAdd(CellCoordinate);
		FSnapshotShardData& SnapshotShard = Cell.SnapshotShards.FindOrAdd(SnapshotShardCoordinate);
		Slot.Entity = Record.Entity;
		Slot.WorldEntityId = Record.WorldEntityId;
		Slot.WorldTransform = Record.WorldTransform;
		Slot.WorldBounds = LocalBounds.TransformBy(Record.WorldTransform);
		Slot.Cell = CellCoordinate;
		Slot.CellIndex = Cell.Trees.Add(Record.Entity);
		Slot.SnapshotShard = SnapshotShardCoordinate;
		Slot.SnapshotShardIndex = SnapshotShard.Trees.Add(Record.Entity);
		Slot.BurnAmount = BurnAmount;
		Slot.bAlive = true;
		MarkUpserted(CellCoordinate, SnapshotShardCoordinate, Record.Entity);
		++ResidentCount;
		++Revision;
	}

	bool CopySlot(const FWorldObjectEntityHandle Entity, FSettlementTreeCandidate& OutTree) const
	{
		if (!Slots.IsValidIndex(Entity.GetSlot()))
		{
			return false;
		}
		const FSlot& Slot = Slots[Entity.GetSlot()];
		if (!Slot.bAlive || Slot.Entity != Entity)
		{
			return false;
		}
		OutTree.Entity = Slot.Entity;
		OutTree.WorldEntityId = Slot.WorldEntityId;
		OutTree.WorldTransform = Slot.WorldTransform;
		OutTree.WorldBounds = Slot.WorldBounds;
		OutTree.Cell = Slot.Cell;
		OutTree.ColorVariation = ComputeSettlementTreeColorVariation(Slot.WorldEntityId);
		OutTree.BurnAmount = Slot.BurnAmount;
		return true;
	}

	bool CommitBurnAmount(const FWorldObjectEntityHandle Entity, const float BurnAmount)
	{
		if (!FMath::IsFinite(BurnAmount) || BurnAmount < 0.0f || BurnAmount > 1.0f
			|| !Slots.IsValidIndex(Entity.GetSlot()))
		{
			return false;
		}
		FSlot& Slot = Slots[Entity.GetSlot()];
		if (!Slot.bAlive || Slot.Entity != Entity)
		{
			return false;
		}
		if (FMath::IsNearlyEqual(Slot.BurnAmount, BurnAmount))
		{
			return true;
		}
		Slot.BurnAmount = BurnAmount;
		MarkUpserted(Slot.Cell, Slot.SnapshotShard, Entity);
		++Revision;
		return true;
	}

	void PublishDirtyCellSnapshots(TArray<FSettlementTreeCellChange>& OutChanges)
	{
		OutChanges.Reset();
		OutChanges.Reserve(DirtySnapshotCells.Num());
		for (const FIntPoint CellCoordinate : DirtySnapshotCells)
		{
			FSettlementTreeCellChange& Change = OutChanges.AddDefaulted_GetRef();
			Change.Cell = CellCoordinate;
			Change.Revision = Revision;
			const FPendingCellChange* Pending = PendingCellChanges.Find(CellCoordinate);
			if (Pending)
			{
				TSharedRef<TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe> Upserts =
					MakeShared<TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe>();
				Upserts->Reserve(Pending->UpsertedEntities.Num());
				for (const FWorldObjectEntityHandle Entity : Pending->UpsertedEntities)
				{
					FSettlementTreeCandidate Candidate;
					if (CopySlot(Entity, Candidate) && Candidate.Cell == CellCoordinate)
					{
						Upserts->Add(MoveTemp(Candidate));
					}
				}
				Change.UpsertedTrees = Upserts;

				TSharedRef<TArray<FWorldObjectEntityHandle>, ESPMode::ThreadSafe> Removals =
					MakeShared<TArray<FWorldObjectEntityHandle>, ESPMode::ThreadSafe>();
				Removals->Reserve(Pending->RemovedEntities.Num());
				for (const FWorldObjectEntityHandle Entity : Pending->RemovedEntities)
				{
					Removals->Add(Entity);
				}
				Change.RemovedEntities = Removals;
			}
			FCell* Cell = Cells.Find(CellCoordinate);
			if (!Cell)
			{
				++CellPublishCount;
				continue;
			}

			// WorldStorage 以 100m Chunk 连续注入。这里只重建真正变化的 100m
			// Snapshot Shard；绝不能为每一批注入复制不断长大的整份 1km Cell。
			if (Pending)
			{
				for (const FIntPoint ShardCoordinate : Pending->DirtySnapshotShards)
				{
					FSnapshotShardData* Shard = Cell->SnapshotShards.Find(ShardCoordinate);
					if (!Shard || Shard->Trees.IsEmpty())
					{
						continue;
					}
					TSharedRef<TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe> PublishedTrees =
						MakeShared<TArray<FSettlementTreeCandidate>, ESPMode::ThreadSafe>();
					PublishedTrees->Reserve(Shard->Trees.Num());
					FBox ShardBounds(ForceInit);
					for (const FWorldObjectEntityHandle Entity : Shard->Trees)
					{
						FSettlementTreeCandidate Candidate;
						if (CopySlot(Entity, Candidate))
						{
							ShardBounds += Candidate.WorldBounds;
							PublishedTrees->Add(MoveTemp(Candidate));
							++SnapshotCandidateCopyCount;
						}
					}
					TSharedRef<FSettlementTreeSnapshotShard, ESPMode::ThreadSafe> PublishedShard =
						MakeShared<FSettlementTreeSnapshotShard, ESPMode::ThreadSafe>();
					PublishedShard->Shard = ShardCoordinate;
					PublishedShard->AggregateBounds = ShardBounds;
					PublishedShard->Trees = PublishedTrees;
					Shard->Snapshot = PublishedShard;
					++SnapshotShardPublishCount;
				}
			}

			TSharedRef<TArray<FSettlementTreeSnapshotShard>, ESPMode::ThreadSafe> PublishedShards =
				MakeShared<TArray<FSettlementTreeSnapshotShard>, ESPMode::ThreadSafe>();
			PublishedShards->Reserve(Cell->SnapshotShards.Num());
			FBox AggregateBounds(ForceInit);
			int32 TreeCount = 0;
			for (const TPair<FIntPoint, FSnapshotShardData>& Pair : Cell->SnapshotShards)
			{
				if (Pair.Value.Snapshot && Pair.Value.Snapshot->Trees && !Pair.Value.Snapshot->Trees->IsEmpty())
				{
					AggregateBounds += Pair.Value.Snapshot->AggregateBounds;
					TreeCount += Pair.Value.Snapshot->Trees->Num();
					PublishedShards->Add(*Pair.Value.Snapshot);
				}
			}
			PublishedShards->Sort(
				[](const FSettlementTreeSnapshotShard& Left, const FSettlementTreeSnapshotShard& Right)
				{
					return Left.Shard.X == Right.Shard.X ? Left.Shard.Y < Right.Shard.Y : Left.Shard.X < Right.Shard.X;
				});
			TSharedRef<FSettlementTreeCellSnapshot, ESPMode::ThreadSafe> Snapshot =
				MakeShared<FSettlementTreeCellSnapshot, ESPMode::ThreadSafe>();
			Snapshot->Cell = CellCoordinate;
			Snapshot->Revision = Revision;
			Snapshot->AggregateBounds = AggregateBounds;
			Snapshot->Shards = PublishedShards;
			Snapshot->TreeCount = TreeCount;
			Cell->Snapshot = Snapshot;
			Change.Snapshot = Snapshot;
			++CellPublishCount;
		}
		DirtySnapshotCells.Reset();
		PendingCellChanges.Reset();
		PublishedRevision = Revision;
	}

	double CellSize = 100000.0;
	FBox LocalBounds = FBox(ForceInit);
	TArray<FSlot> Slots;
	TMap<FIntPoint, FCell> Cells;
	TSet<FIntPoint> DirtySnapshotCells;
	TMap<FIntPoint, FPendingCellChange> PendingCellChanges;
	int32 ResidentCount = 0;
	uint64 Revision = 1;
	uint64 PublishedRevision = 1;
	uint64 CellPublishCount = 0;
	uint64 SnapshotCandidateCopyCount = 0;
	uint64 SnapshotShardPublishCount = 0;
	double LastPublishMilliseconds = 0.0;
};

USettlementTreeWorldSubsystem::USettlementTreeWorldSubsystem() = default;
USettlementTreeWorldSubsystem::~USettlementTreeWorldSubsystem() = default;

void USettlementTreeWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Data = MakePimpl<FSettlementTreeWorldData>();
	if (const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>())
	{
		Data->CellSize = FMath::Max(1000.0, Settings->TreeCellSize);
	}
	UWorldObjectWorldSubsystem* WorldObjectSubsystem = GetWorldRef().GetSubsystem<UWorldObjectWorldSubsystem>();
	WorldObjects = WorldObjectSubsystem;
	Definition = NewObject<USettlementTreeDefinition>(this, TEXT("SettlementTreeDefinition"));
	if (!WorldObjectSubsystem || !Definition || !WorldObjectSubsystem->RegisterDefinition(*Definition))
	{
		UE_LOG(LogElementSandboxWorldObjectCatalog, Error, TEXT("Settlement.Tree Definition 注册失败。"));
		return;
	}
	Data->LocalBounds = Definition->InteractionLocalBounds;
	UpsertedHandle =
		WorldObjectSubsystem->OnEntitiesUpserted().AddUObject(this, &USettlementTreeWorldSubsystem::HandleUpserted);
	RuntimeEvictedHandle = WorldObjectSubsystem->OnEntitiesRuntimeEvicted().AddUObject(
		this, &USettlementTreeWorldSubsystem::HandleRemoved);
	GameplayDestroyedHandle = WorldObjectSubsystem->OnEntitiesGameplayDestroyed().AddUObject(
		this, &USettlementTreeWorldSubsystem::HandleRemoved);
}

void USettlementTreeWorldSubsystem::Deinitialize()
{
	if (UWorldObjectWorldSubsystem* WorldObjectSubsystem = WorldObjects.Get())
	{
		WorldObjectSubsystem->OnEntitiesUpserted().Remove(UpsertedHandle);
		WorldObjectSubsystem->OnEntitiesRuntimeEvicted().Remove(RuntimeEvictedHandle);
		WorldObjectSubsystem->OnEntitiesGameplayDestroyed().Remove(GameplayDestroyedHandle);
	}
	WorldObjects.Reset();
	Definition = nullptr;
	Data.Reset();
	Super::Deinitialize();
}

bool USettlementTreeWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USettlementTreeWorldSubsystem::Tick(const float DeltaTime)
{
	(void)DeltaTime;
	const double Start = FPlatformTime::Seconds();
	if (Data && !Data->DirtySnapshotCells.IsEmpty())
	{
		TArray<FSettlementTreeCellChange> Changes;
		Data->PublishDirtyCellSnapshots(Changes);
		if (!Changes.IsEmpty())
		{
			CellsPublishedDelegate.Broadcast(Changes);
		}
	}
	if (Data)
	{
		Data->LastPublishMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	}
	CSV_CUSTOM_STAT(SettlementTrees, CatalogPublishMilliseconds, Data ? Data->LastPublishMilliseconds : 0.0,
					ECsvCustomStatOp::Set);
}

bool USettlementTreeWorldSubsystem::IsTickable() const
{
	return Data && !Data->DirtySnapshotCells.IsEmpty();
}

TStatId USettlementTreeWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USettlementTreeWorldSubsystem, STATGROUP_Tickables);
}

void USettlementTreeWorldSubsystem::HandleUpserted(const TConstArrayView<FWorldObjectLifecycleRecord> Records)
{
	if (!Data)
	{
		return;
	}
	for (const FWorldObjectLifecycleRecord& Record : Records)
	{
		if (Record.DefinitionId == SettlementTreeDefinitionId &&
			Record.SpatialClass == EWorldObjectSpatialClass::PermanentStatic)
		{
				// Element 状态在 Dependent 恢复阶段通过 Outbox 再投影；宿主 Primary 恢复不读取元素状态。
				Data->Upsert(Record, 0.0f);
		}
	}
}

void USettlementTreeWorldSubsystem::HandleRemoved(const TConstArrayView<FWorldObjectLifecycleRecord> Records)
{
	if (!Data)
	{
		return;
	}
	for (const FWorldObjectLifecycleRecord& Record : Records)
	{
		if (Record.DefinitionId == SettlementTreeDefinitionId)
		{
			Data->Remove(Record.Entity);
		}
	}
}

bool USettlementTreeWorldSubsystem::TryGetTree(const FWorldObjectEntityHandle Entity,
											   FSettlementTreeCandidate& OutTree) const
{
	return Data && Data->CopySlot(Entity, OutTree);
}

bool USettlementTreeWorldSubsystem::CommitBurnAmount(
	const FWorldObjectEntityHandle Entity,
	const float BurnAmount)
{
	check(IsInGameThread());
	return Data && Data->CommitBurnAmount(Entity, BurnAmount);
}

void USettlementTreeWorldSubsystem::CopyCellSnapshots(TArray<FSettlementTreeCellSnapshot>& OutCells,
													  uint64& OutRevision) const
{
	OutCells.Reset();
	OutRevision = Data ? Data->PublishedRevision : 0;
	if (!Data)
	{
		return;
	}
	OutCells.Reserve(Data->Cells.Num());
	for (const TPair<FIntPoint, FSettlementTreeWorldData::FCell>& Pair : Data->Cells)
	{
		if (!Pair.Value.Snapshot || !Pair.Value.Snapshot->Shards || Pair.Value.Snapshot->TreeCount == 0)
		{
			continue;
		}
		OutCells.Add(*Pair.Value.Snapshot);
	}
}

void USettlementTreeWorldSubsystem::QueryTrees(const FBox& Bounds, TArray<FSettlementTreeCandidate>& OutTrees) const
{
	OutTrees.Reset();
	if (!Data || Bounds.IsValid == 0 || Bounds.ContainsNaN())
	{
		return;
	}
	const FIntPoint MinCell = ResolveTreeCell(Bounds.Min, Data->CellSize);
	const FIntPoint MaxCell = ResolveTreeCell(Bounds.Max, Data->CellSize);
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			const FSettlementTreeWorldData::FCell* Cell = Data->Cells.Find(FIntPoint(X, Y));
			if (!Cell)
			{
				continue;
			}
			for (const FWorldObjectEntityHandle Entity : Cell->Trees)
			{
				FSettlementTreeCandidate Candidate;
				if (Data->CopySlot(Entity, Candidate) && Bounds.Intersect(Candidate.WorldBounds))
				{
					OutTrees.Add(MoveTemp(Candidate));
				}
			}
		}
	}
}

FSettlementTreeCatalogStats USettlementTreeWorldSubsystem::GetStats() const
{
	FSettlementTreeCatalogStats Stats;
	if (Data)
	{
		Stats.ResidentTreeCount = Data->ResidentCount;
		Stats.CellCount = Data->Cells.Num();
		Stats.Revision = static_cast<int64>(Data->Revision);
		Stats.PublishedRevision = static_cast<int64>(Data->PublishedRevision);
		Stats.CellPublishCount = static_cast<int64>(Data->CellPublishCount);
		Stats.SnapshotCandidateCopyCount = static_cast<int64>(Data->SnapshotCandidateCopyCount);
		Stats.SnapshotShardPublishCount = static_cast<int64>(Data->SnapshotShardPublishCount);
		Stats.LastPublishMilliseconds = Data->LastPublishMilliseconds;
	}
	return Stats;
}
