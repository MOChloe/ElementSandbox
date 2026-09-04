#include "Rendering/BuildPresentationIndex.h"

#include "Algo/Sort.h"
#include "Async/Async.h"
#include "Containers/ArrayView.h"
#include "Containers/Queue.h"
#include "PresentationViewSource.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 PresentationBVHLeafSize = 8;
constexpr int32 MaximumConcurrentPresentationPackedBuilds = 1;

bool IsValidPresentationBounds(const FBox& Bounds)
{
	return Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN();
}

bool SelectorEntryLess(const FBuildPresentationSelectorEntry& Left, const FBuildPresentationSelectorEntry& Right)
{
	const FVector LeftCenter = Left.Bounds.GetCenter();
	const FVector RightCenter = Right.Bounds.GetCenter();
	if (LeftCenter.X != RightCenter.X)
	{
		return LeftCenter.X < RightCenter.X;
	}
	if (LeftCenter.Y != RightCenter.Y)
	{
		return LeftCenter.Y < RightCenter.Y;
	}
	if (LeftCenter.Z != RightCenter.Z)
	{
		return LeftCenter.Z < RightCenter.Z;
	}
	if (Left.Entity.GetRegistryId() != Right.Entity.GetRegistryId())
	{
		return Left.Entity.GetRegistryId() < Right.Entity.GetRegistryId();
	}
	if (Left.Entity.GetIndex() != Right.Entity.GetIndex())
	{
		return Left.Entity.GetIndex() < Right.Entity.GetIndex();
	}
	return Left.Entity.GetGeneration() < Right.Entity.GetGeneration();
}

int32 BuildPackedNode(FBuildPresentationCellSnapshot& Snapshot, const int32 Start, const int32 Count)
{
	const int32 NodeIndex = Snapshot.Nodes.AddDefaulted();
	FBuildPresentationSelectorNode& Node = Snapshot.Nodes[NodeIndex];
	Node.FirstEntry = Start;
	Node.EntryCount = Count;
	if (Count <= PresentationBVHLeafSize)
	{
		for (int32 Index = Start; Index < Start + Count; ++Index)
		{
			const FBuildPresentationSelectorEntry& Entry = Snapshot.OrderedEntries[Index];
			Node.Bounds += Entry.Bounds;
			Node.MeshPartCost += Entry.MeshPartCost;
			Node.MinimumMeshPartCost = FMath::Min(Node.MinimumMeshPartCost, Entry.MeshPartCost);
		}
		return NodeIndex;
	}

	const int32 LeftCount = Count / 2;
	const int32 LeftChild = BuildPackedNode(Snapshot, Start, LeftCount);
	const int32 RightChild = BuildPackedNode(Snapshot, Start + LeftCount, Count - LeftCount);
	FBuildPresentationSelectorNode& FinalNode = Snapshot.Nodes[NodeIndex];
	FinalNode.LeftChild = LeftChild;
	FinalNode.RightChild = RightChild;
	FinalNode.Bounds = Snapshot.Nodes[LeftChild].Bounds + Snapshot.Nodes[RightChild].Bounds;
	FinalNode.MeshPartCost = Snapshot.Nodes[LeftChild].MeshPartCost + Snapshot.Nodes[RightChild].MeshPartCost;
	FinalNode.MinimumMeshPartCost =
		FMath::Min(Snapshot.Nodes[LeftChild].MinimumMeshPartCost, Snapshot.Nodes[RightChild].MinimumMeshPartCost);
	return NodeIndex;
}

TSharedRef<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe> BuildPackedSnapshot(
	const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>& BaseSnapshot,
	const TConstArrayView<TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>> DeltaBlocks,
	const TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe>& StaticEntryTombstones,
	const uint64 Revision)
{
	TSharedRef<FBuildPresentationCellSnapshot, ESPMode::ThreadSafe> Snapshot =
		MakeShared<FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>();
	int32 EntryCount = BaseSnapshot.IsValid() ? BaseSnapshot->OrderedEntries.Num() : 0;
	for (const TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>& Block : DeltaBlocks)
	{
		EntryCount += Block.IsValid() ? Block->Num() : 0;
	}
	Snapshot->OrderedEntries.Reserve(EntryCount);
	const auto AppendLiveEntries = [&Snapshot, &StaticEntryTombstones](
		const TConstArrayView<FBuildPresentationSelectorEntry> Entries)
	{
		for (const FBuildPresentationSelectorEntry& Entry : Entries)
		{
			const bool bTombstoned = Entry.StaticSnapshotSerial != INDEX_NONE &&
				StaticEntryTombstones.IsValid() &&
				StaticEntryTombstones->IsValidIndex(Entry.StaticSnapshotSerial) &&
				(*StaticEntryTombstones)[Entry.StaticSnapshotSerial];
			if (!bTombstoned)
			{
				Snapshot->OrderedEntries.Add(Entry);
			}
		}
	};
	if (BaseSnapshot.IsValid())
	{
		AppendLiveEntries(BaseSnapshot->OrderedEntries);
	}
	for (const TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>& Block : DeltaBlocks)
	{
		if (Block.IsValid())
		{
			AppendLiveEntries(*Block);
		}
	}
	Snapshot->OrderedEntries.Sort(SelectorEntryLess);
	Snapshot->Nodes.Reserve(Snapshot->OrderedEntries.Num() * 2);
	if (!Snapshot->OrderedEntries.IsEmpty())
	{
		BuildPackedNode(*Snapshot, 0, Snapshot->OrderedEntries.Num());
	}
	Snapshot->Revision = Revision;
	return Snapshot;
}
} // namespace

struct FBuildPresentationPackedBuildResult final
{
	FIntVector CellCoordinate = FIntVector::ZeroValue;
	uint64 BuildId = 0;
	uint64 StructuralRevision = 0;
	int32 CapturedDeltaEntryCount = 0;
	int32 CapturedTombstoneCount = 0;
	TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe> Snapshot;
};

class FBuildPresentationPackedAsyncState final
	: public TSharedFromThis<FBuildPresentationPackedAsyncState, ESPMode::ThreadSafe>
{
public:
	TAtomic<bool> bActive{true};
	TAtomic<int32> InFlightCount{0};
	TQueue<TUniquePtr<FBuildPresentationPackedBuildResult>, EQueueMode::Mpsc> CompletedResults;
};

bool TryGetPresentationGridCoordinate(const FVector& WorldLocation, const double CellSize, FIntVector& OutCoordinate)
{
	if (WorldLocation.ContainsNaN() || !FMath::IsFinite(CellSize) || CellSize <= 0.0)
	{
		return false;
	}
	const FVector Coordinate(FMath::FloorToDouble(WorldLocation.X / CellSize),
							 FMath::FloorToDouble(WorldLocation.Y / CellSize),
							 FMath::FloorToDouble(WorldLocation.Z / CellSize));
	if (Coordinate.ContainsNaN() || Coordinate.X < MIN_int32 || Coordinate.X > MAX_int32 || Coordinate.Y < MIN_int32 ||
		Coordinate.Y > MAX_int32 || Coordinate.Z < MIN_int32 || Coordinate.Z > MAX_int32)
	{
		return false;
	}
	OutCoordinate = FIntVector(static_cast<int32>(Coordinate.X), static_cast<int32>(Coordinate.Y),
							   static_cast<int32>(Coordinate.Z));
	return true;
}

FBuildPresentationIndex::FBuildPresentationIndex(const FBuildPresentationResidencyConfig& Config)
	: StaticCellSize(Config.StaticCellSize), GameplayChunkSize(Config.GameplayChunkSize),
	  PackedAsyncState(MakeShared<FBuildPresentationPackedAsyncState, ESPMode::ThreadSafe>())
{
}

FBuildPresentationIndex::~FBuildPresentationIndex()
{
	if (PackedAsyncState)
	{
		PackedAsyncState->bActive.Store(false);
	}
}

FPresentationEntry* FBuildPresentationIndex::FindEntry(const FBuildEntityHandle Entity)
{
	if (!Entity.IsSet() || !EntriesBySlot.IsValidIndex(Entity.GetIndex()))
	{
		return nullptr;
	}
	FPresentationEntry& Entry = EntriesBySlot[Entity.GetIndex()];
	return Entry.Entity == Entity ? &Entry : nullptr;
}

const FPresentationEntry* FBuildPresentationIndex::FindEntry(const FBuildEntityHandle Entity) const
{
	return const_cast<FBuildPresentationIndex*>(this)->FindEntry(Entity);
}

void FBuildPresentationIndex::BumpIndexRevision()
{
	if (++IndexRevision == 0)
	{
		++IndexRevision;
	}
}

void FBuildPresentationIndex::AppendStaticDeltaBlock(
	FPresentationStaticCell& Cell, TSharedRef<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe> Block)
{
	if (Block->IsEmpty())
	{
		return;
	}
	Cell.PendingDeltaEntryCount += Block->Num();
	Cell.PendingDeltaBlocks.Add(MoveTemp(Block));
}

void FBuildPresentationIndex::RemoveEntry(const FBuildEntityHandle Entity)
{
	FPresentationEntry* Entry = FindEntry(Entity);
	if (!Entry)
	{
		return;
	}
	if (Entry->bPackedStatic)
	{
		FPresentationStaticCell* Cell = StaticCells.Find(Entry->Cell);
		if (Cell && Cell->EntrySlots.IsValidIndex(Entry->IndexInCell))
		{
			if (StaticEntryTombstones.IsValidIndex(Entry->StaticSnapshotSerial))
			{
				StaticEntryTombstones[Entry->StaticSnapshotSerial] = true;
				bStaticEntryTombstoneSnapshotDirty = true;
				++Cell->TombstonedEntryCount;
			}
			const int32 LastIndex = Cell->EntrySlots.Num() - 1;
			if (Entry->IndexInCell != LastIndex)
			{
				const int32 MovedSlot = Cell->EntrySlots[LastIndex];
				Cell->EntrySlots[Entry->IndexInCell] = MovedSlot;
				EntriesBySlot[MovedSlot].IndexInCell = Entry->IndexInCell;
			}
			Cell->EntrySlots.Pop(EAllowShrinking::No);
			Cell->EntityCount = FMath::Max(0, Cell->EntityCount - 1);
			Cell->MeshPartCount = FMath::Max(0, Cell->MeshPartCount - Entry->MeshPartCost);
			Cell->DynamicPartCount = FMath::Max(0, Cell->DynamicPartCount - Entry->DynamicPartCost);
			Cell->EstimatedBytes = Cell->EntrySlots.GetAllocatedSize();
			if (++Cell->Revision == 0)
			{
				++Cell->Revision;
			}
			if (Cell->EntrySlots.IsEmpty())
			{
				StaticCells.Remove(Entry->Cell);
			}
		}
	}
	else
	{
		FPresentationMutableChunk* Chunk = MutableChunks.Find(Entry->Cell);
		if (Chunk && Chunk->EntrySlots.IsValidIndex(Entry->IndexInCell))
		{
			const int32 LastIndex = Chunk->EntrySlots.Num() - 1;
			if (Entry->IndexInCell != LastIndex)
			{
				const int32 MovedSlot = Chunk->EntrySlots[LastIndex];
				Chunk->EntrySlots[Entry->IndexInCell] = MovedSlot;
				EntriesBySlot[MovedSlot].IndexInCell = Entry->IndexInCell;
			}
			Chunk->EntrySlots.Pop(EAllowShrinking::No);
			Chunk->EntityCount = FMath::Max(0, Chunk->EntityCount - 1);
			Chunk->MeshPartCount = FMath::Max(0, Chunk->MeshPartCount - Entry->MeshPartCost);
			Chunk->DynamicPartCount = FMath::Max(0, Chunk->DynamicPartCount - Entry->DynamicPartCost);
			Chunk->EstimatedBytes = Chunk->EntrySlots.GetAllocatedSize();
			if (++Chunk->Revision == 0)
			{
				++Chunk->Revision;
			}
			if (Chunk->EntrySlots.IsEmpty())
			{
				MutableChunks.Remove(Entry->Cell);
			}
		}
	}
	*Entry = FPresentationEntry();
	BumpIndexRevision();
}

bool FBuildPresentationIndex::UpsertEntry(const FBuildEntityHandle Entity, const FBox& Bounds, const int32 MeshPartCost,
										  const int32 DynamicPartCost, const bool bPackedStatic)
{
	if (!Entity.IsSet() || !IsValidPresentationBounds(Bounds) || MeshPartCost <= 0 || DynamicPartCost < 0 ||
		DynamicPartCost > MeshPartCost)
	{
		return false;
	}
	FIntVector NewCell = FIntVector::ZeroValue;
	const double CellSize = bPackedStatic ? StaticCellSize : GameplayChunkSize;
	if (!TryGetPresentationGridCoordinate(Bounds.GetCenter(), CellSize, NewCell))
	{
		return false;
	}
	if (FPresentationEntry* Existing = FindEntry(Entity))
	{
		if (Existing->Bounds == Bounds && Existing->MeshPartCost == MeshPartCost &&
			Existing->DynamicPartCost == DynamicPartCost && Existing->bPackedStatic == bPackedStatic &&
			Existing->Cell == NewCell)
		{
			return true;
		}
		RemoveEntry(Entity);
	}
	if (!EntriesBySlot.IsValidIndex(Entity.GetIndex()))
	{
		EntriesBySlot.SetNum(Entity.GetIndex() + 1, EAllowShrinking::No);
	}
	FPresentationEntry& Entry = EntriesBySlot[Entity.GetIndex()];
	Entry.Entity = Entity;
	Entry.Bounds = Bounds;
	Entry.MeshPartCost = MeshPartCost;
	Entry.DynamicPartCost = DynamicPartCost;
	Entry.Cell = NewCell;
	Entry.bPackedStatic = bPackedStatic;
	if (bPackedStatic)
	{
		Entry.StaticSnapshotSerial = StaticEntryTombstones.Num();
		StaticEntryTombstones.Add(false);
		FPresentationStaticCell& Cell = StaticCells.FindOrAdd(NewCell);
		Entry.IndexInCell = Cell.EntrySlots.Add(Entity.GetIndex());
		Cell.Bounds += Bounds;
		++Cell.EntityCount;
		Cell.MeshPartCount += MeshPartCost;
		Cell.DynamicPartCount += DynamicPartCost;
		Cell.DeltaBounds += Bounds;
		Cell.DeltaMeshPartCost += MeshPartCost;
		Cell.DeltaMinimumMeshPartCost = FMath::Min(Cell.DeltaMinimumMeshPartCost, MeshPartCost);
		Cell.EstimatedBytes = Cell.EntrySlots.GetAllocatedSize();
		if (++Cell.Revision == 0)
		{
			++Cell.Revision;
		}
		FPresentationSelectorEntryBlock DeltaBlock;
		DeltaBlock.Add({Entity, Bounds, MeshPartCost, 0.0, Entry.StaticSnapshotSerial});
		AppendStaticDeltaBlock(
			Cell, MakeShared<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>(MoveTemp(DeltaBlock)));
	}
	else
	{
		FPresentationMutableChunk& Chunk = MutableChunks.FindOrAdd(NewCell);
		Entry.IndexInCell = Chunk.EntrySlots.Add(Entity.GetIndex());
		Chunk.Bounds += Bounds;
		++Chunk.EntityCount;
		Chunk.MeshPartCount += MeshPartCost;
		Chunk.DynamicPartCount += DynamicPartCost;
		Chunk.MinimumMeshPartCost = FMath::Min(Chunk.MinimumMeshPartCost, MeshPartCost);
		Chunk.EstimatedBytes = Chunk.EntrySlots.GetAllocatedSize();
		if (++Chunk.Revision == 0)
		{
			++Chunk.Revision;
		}
	}
	BumpIndexRevision();
	return true;
}

void FBuildPresentationIndex::ProcessAsyncPackedBuildWork(const bool bBuildSynchronouslyForTesting)
{
	check(IsInGameThread());
	if (!PackedAsyncState || !PackedAsyncState->bActive.Load())
	{
		return;
	}

	const int32 PublishBudget = bBuildSynchronouslyForTesting ? MAX_int32 : 1;
	for (int32 PublishIndex = 0; PublishIndex < PublishBudget; ++PublishIndex)
	{
		TUniquePtr<FBuildPresentationPackedBuildResult> Result;
		if (!PackedAsyncState->CompletedResults.Dequeue(Result) || !Result)
		{
			break;
		}
		FPresentationStaticCell* Cell = StaticCells.Find(Result->CellCoordinate);
		if (!Cell || Cell->PackedBuildId != Result->BuildId || Cell->StructuralRevision != Result->StructuralRevision)
		{
			continue;
		}
		Cell->bPackedBuildInFlight = false;
		Cell->PublishedSnapshot = MoveTemp(Result->Snapshot);
		Cell->PackedEntryCount = Cell->PublishedSnapshot.IsValid() ? Cell->PublishedSnapshot->OrderedEntries.Num() : 0;
		Cell->InFlightDeltaBlocks.Reset();
		Cell->InFlightDeltaEntryCount = 0;
		Cell->TombstonedEntryCount = FMath::Max(0, Cell->TombstonedEntryCount - Result->CapturedTombstoneCount);
		Cell->DeltaBounds = FBox(ForceInit);
		Cell->DeltaMeshPartCost = 0;
		Cell->DeltaMinimumMeshPartCost = MAX_int32;
		++StaticBVHBuildCount;
	}

	while (PackedAsyncState->InFlightCount.Load() < MaximumConcurrentPresentationPackedBuilds)
	{
		FIntVector SelectedCoordinate = FIntVector::ZeroValue;
		FPresentationStaticCell* SelectedCell = nullptr;
		for (TPair<FIntVector, FPresentationStaticCell>& Pair : StaticCells)
		{
			FPresentationStaticCell& Cell = Pair.Value;
			const int32 RebuildThreshold =
				FMath::Max(FPresentationStaticCell::MinimumPackedRebuildDeltaEntries, Cell.PackedEntryCount);
			const int32 ChangedEntryCount = Cell.PendingDeltaEntryCount + Cell.TombstonedEntryCount;
			const bool bEligible =
				!Cell.bPackedBuildInFlight && ChangedEntryCount > 0 &&
				(!Cell.PublishedSnapshot.IsValid() || ChangedEntryCount >= RebuildThreshold);
			if (bEligible &&
				(!SelectedCell || ChangedEntryCount >
					SelectedCell->PendingDeltaEntryCount + SelectedCell->TombstonedEntryCount))
			{
				SelectedCoordinate = Pair.Key;
				SelectedCell = &Cell;
			}
		}
		if (!SelectedCell)
		{
			break;
		}

		SelectedCell->InFlightDeltaBlocks = MoveTemp(SelectedCell->PendingDeltaBlocks);
		SelectedCell->InFlightDeltaEntryCount = SelectedCell->PendingDeltaEntryCount;
		SelectedCell->PendingDeltaEntryCount = 0;
		SelectedCell->bPackedBuildInFlight = true;
		const uint64 BuildId = ++SelectedCell->PackedBuildId;
		const uint64 StructuralRevision = SelectedCell->StructuralRevision;
		const int32 CapturedDeltaEntryCount = SelectedCell->InFlightDeltaEntryCount;
		const int32 CapturedTombstoneCount = SelectedCell->TombstonedEntryCount;
		const TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe> BaseSnapshot =
			SelectedCell->PublishedSnapshot;
		const TArray<TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>> CapturedBlocks =
			SelectedCell->InFlightDeltaBlocks;
		const TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> CapturedTombstones =
			GetStaticEntryTombstoneSnapshot();

		if (bBuildSynchronouslyForTesting)
		{
			SelectedCell->PublishedSnapshot =
				BuildPackedSnapshot(BaseSnapshot, CapturedBlocks, CapturedTombstones, SelectedCell->Revision);
			SelectedCell->PackedEntryCount = SelectedCell->PublishedSnapshot->OrderedEntries.Num();
			SelectedCell->InFlightDeltaBlocks.Reset();
			SelectedCell->InFlightDeltaEntryCount = 0;
			SelectedCell->TombstonedEntryCount =
				FMath::Max(0, SelectedCell->TombstonedEntryCount - CapturedTombstoneCount);
			SelectedCell->bPackedBuildInFlight = false;
			++StaticBVHBuildCount;
			continue;
		}

		TSharedRef<FBuildPresentationPackedAsyncState, ESPMode::ThreadSafe> SharedAsyncState =
			PackedAsyncState.ToSharedRef();
		++SharedAsyncState->InFlightCount;
		Async(EAsyncExecution::ThreadPool,
			  [SharedAsyncState, SelectedCoordinate, BuildId, StructuralRevision, CapturedDeltaEntryCount,
			   CapturedTombstoneCount, BaseSnapshot, CapturedBlocks, CapturedTombstones]() mutable
			  {
				  TRACE_CPUPROFILER_EVENT_SCOPE(Building_Presentation_PackedBVHBuild);
				  TUniquePtr<FBuildPresentationPackedBuildResult> Result =
					  MakeUnique<FBuildPresentationPackedBuildResult>();
				  Result->CellCoordinate = SelectedCoordinate;
				  Result->BuildId = BuildId;
				  Result->StructuralRevision = StructuralRevision;
				  Result->CapturedDeltaEntryCount = CapturedDeltaEntryCount;
				  Result->CapturedTombstoneCount = CapturedTombstoneCount;
				  Result->Snapshot = BuildPackedSnapshot(BaseSnapshot, CapturedBlocks, CapturedTombstones, BuildId);
				  if (SharedAsyncState->bActive.Load())
				  {
					  SharedAsyncState->CompletedResults.Enqueue(MoveTemp(Result));
				  }
				  --SharedAsyncState->InFlightCount;
			  });
		break;
	}
}

bool FBuildPresentationIndex::HasPendingAsyncPackedBuildWork() const
{
	if (!PackedAsyncState)
	{
		return false;
	}
	if (PackedAsyncState->InFlightCount.Load() > 0 || !PackedAsyncState->CompletedResults.IsEmpty())
	{
		return true;
	}
	for (const TPair<FIntVector, FPresentationStaticCell>& Pair : StaticCells)
	{
		const FPresentationStaticCell& Cell = Pair.Value;
		const int32 RebuildThreshold =
			FMath::Max(FPresentationStaticCell::MinimumPackedRebuildDeltaEntries, Cell.PackedEntryCount);
		const int32 ChangedEntryCount = Cell.PendingDeltaEntryCount + Cell.TombstonedEntryCount;
		if (!Cell.bPackedBuildInFlight && ChangedEntryCount > 0 &&
			(!Cell.PublishedSnapshot.IsValid() || ChangedEntryCount >= RebuildThreshold))
		{
			return true;
		}
	}
	return false;
}

void FBuildPresentationIndex::ResetPackedAsyncState()
{
	if (PackedAsyncState)
	{
		PackedAsyncState->bActive.Store(false);
	}
	PackedAsyncState = MakeShared<FBuildPresentationPackedAsyncState, ESPMode::ThreadSafe>();
}

void FBuildPresentationIndex::Clear()
{
	EntriesBySlot.Reset();
	StaticCells.Reset();
	MutableChunks.Reset();
	StaticEntryTombstones.Reset();
	PublishedStaticEntryTombstones.Reset();
	bStaticEntryTombstoneSnapshotDirty = false;
	ResetPackedAsyncState();
	BumpIndexRevision();
}

void FBuildPresentationIndex::ReserveEntityCapacity(const int32 EntityCapacity)
{
	if (EntityCapacity > 0)
	{
		EntriesBySlot.Reserve(EntityCapacity);
	}
}

template <typename RequestType> void FBuildPresentationIndex::CaptureSelectionSourcesImpl(RequestType& OutRequest) const
{
	OutRequest.StaticEntryTombstones = GetStaticEntryTombstoneSnapshot();
	OutRequest.StaticCells.Reserve(StaticCells.Num());
	for (const TPair<FIntVector, FPresentationStaticCell>& Pair : StaticCells)
	{
		const FPresentationStaticCell& Cell = Pair.Value;
		if (Cell.PublishedSnapshot.IsValid())
		{
			OutRequest.StaticCells.Add(Cell.PublishedSnapshot);
		}
		OutRequest.StaticDeltaBlocks.Append(Cell.InFlightDeltaBlocks);
		OutRequest.StaticDeltaBlocks.Append(Cell.PendingDeltaBlocks);
	}
	for (const TPair<FIntVector, FPresentationMutableChunk>& Pair : MutableChunks)
	{
		for (const int32 EntrySlot : Pair.Value.EntrySlots)
		{
			if (!EntriesBySlot.IsValidIndex(EntrySlot))
			{
				continue;
			}
			const FPresentationEntry& Entry = EntriesBySlot[EntrySlot];
			if (Entry.Entity.IsSet())
			{
				OutRequest.MutableEntries.Add({Entry.Entity, Entry.Bounds, Entry.MeshPartCost, 0.0});
			}
		}
	}
}

TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> FBuildPresentationIndex::GetStaticEntryTombstoneSnapshot() const
{
	check(IsInGameThread());
	if (bStaticEntryTombstoneSnapshotDirty)
	{
		PublishedStaticEntryTombstones =
			MakeShared<TBitArray<>, ESPMode::ThreadSafe>(StaticEntryTombstones);
		bStaticEntryTombstoneSnapshotDirty = false;
	}
	return PublishedStaticEntryTombstones;
}

void FBuildPresentationIndex::CaptureSelectionSources(FBuildLocalSelectionRequest& OutRequest) const
{
	CaptureSelectionSourcesImpl(OutRequest);
}

void FBuildPresentationIndex::CaptureSelectionSources(FBuildFarSelectionRequest& OutRequest) const
{
	CaptureSelectionSourcesImpl(OutRequest);
}

void FBuildPresentationIndex::GatherHotPinnedEntities(const TConstArrayView<FPresentationViewSource> Views,
													  const double Radius, TSet<FBuildEntityHandle>& OutEntities) const
{
	const double RadiusSquared = FMath::Square(FMath::Max(0.0, Radius));
	for (const FPresentationEntry& Entry : EntriesBySlot)
	{
		if (!Entry.Entity.IsSet())
		{
			continue;
		}
		for (const FPresentationViewSource& View : Views)
		{
			if (Entry.Bounds.ComputeSquaredDistanceToPoint(View.SubjectLocation) <= RadiusSquared)
			{
				OutEntities.Add(Entry.Entity);
				break;
			}
		}
	}
}

FBuildPresentationIndexStats FBuildPresentationIndex::GetStats() const
{
	FBuildPresentationIndexStats Stats;
	Stats.EstimatedAllocatedSize =
		EntriesBySlot.GetAllocatedSize() + StaticCells.GetAllocatedSize() + MutableChunks.GetAllocatedSize() +
		StaticEntryTombstones.GetAllocatedSize();
	Stats.StaticCellCount = StaticCells.Num();
	Stats.MutableChunkCount = MutableChunks.Num();
	Stats.StaticBVHBuildCount = StaticBVHBuildCount;
	Stats.IndexRevision = IndexRevision;
	for (const TPair<FIntVector, FPresentationStaticCell>& Pair : StaticCells)
	{
		const FPresentationStaticCell& Cell = Pair.Value;
		Stats.StaticPackedEntryCount += Cell.PackedEntryCount;
		Stats.StaticDeltaEntryCount += Cell.GetDeltaEntryCount();
		Stats.EstimatedAllocatedSize += Cell.EstimatedBytes;
		if (Cell.PublishedSnapshot.IsValid())
		{
			Stats.EstimatedAllocatedSize += Cell.PublishedSnapshot->OrderedEntries.GetAllocatedSize();
			Stats.EstimatedAllocatedSize += Cell.PublishedSnapshot->Nodes.GetAllocatedSize();
		}
	}
	for (const TPair<FIntVector, FPresentationMutableChunk>& Pair : MutableChunks)
	{
		Stats.EstimatedAllocatedSize += Pair.Value.EstimatedBytes;
	}
	return Stats;
}
