#include "Spatial/BuildSpatialIndex.h"

#include "ElementSandboxBuilding.h"
#include "Spatial/BuildSpatialChunk.h"
#include "Spatial/BuildSpatialEntry.h"
#include "Storage/BuildStableArrayAllocator.h"

#include "Async/Async.h"
#include "Containers/Queue.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Templates/Atomic.h"

using namespace UE::ElementSandbox::Building::Private;

namespace
{
	struct FBuildSpatialEntityRecord
	{
		FBuildEntityHandle Entity;
		FBox Bounds = FBox(ForceInit);
		TArray<FIntVector, TInlineAllocator<4>> Chunks;
		EBuildSpatialMobility Mobility = EBuildSpatialMobility::Static;
	};

	struct FBuildSpatialSnapshotResult final
	{
		FIntVector ChunkCoordinate = FIntVector::ZeroValue;
		uint64 StaticVersion = 0;
		FBuildAABBTree Snapshot;
	};

	class FBuildSpatialAsyncState final
		: public TSharedFromThis<FBuildSpatialAsyncState, ESPMode::ThreadSafe>
	{
	public:
		TAtomic<bool> bActive{true};
		TAtomic<int32> InFlightCount{0};
		TQueue<TUniquePtr<FBuildSpatialSnapshotResult>, EQueueMode::Mpsc> CompletedResults;
	};

	struct FBuildSpatialChunkAsyncState final
	{
		bool bInFlight = false;
		bool bReadyQueued = false;
		int32 StaleRetryDeltaThreshold = 0;
	};

	struct FBuildSpatialIdleDeadline final
	{
		FIntVector ChunkCoordinate = FIntVector::ZeroValue;
		double DeadlineSeconds = 0.0;
	};

	/** 每个 Chunk 至多一个节点的可更新最小堆。 */
	class FBuildSpatialIdleDeadlineHeap final
	{
	public:
		void AddOrUpdate(const FIntVector& ChunkCoordinate, const double DeadlineSeconds)
		{
			if (int32* ExistingIndex = IndexByChunk.Find(ChunkCoordinate))
			{
				const int32 Index = *ExistingIndex;
				const double PreviousDeadline = Nodes[Index].DeadlineSeconds;
				Nodes[Index].DeadlineSeconds = DeadlineSeconds;
				if (DeadlineSeconds < PreviousDeadline)
				{
					SiftUp(Index);
				}
				else if (DeadlineSeconds > PreviousDeadline)
				{
					SiftDown(Index);
				}
				return;
			}

			const int32 NewIndex = Nodes.Add({ChunkCoordinate, DeadlineSeconds});
			IndexByChunk.Add(ChunkCoordinate, NewIndex);
			SiftUp(NewIndex);
		}

		void Remove(const FIntVector& ChunkCoordinate)
		{
			const int32* ExistingIndex = IndexByChunk.Find(ChunkCoordinate);
			if (ExistingIndex)
			{
				RemoveAt(*ExistingIndex);
			}
		}

		bool PopDue(
			const double CurrentTimeSeconds,
			FIntVector& OutChunkCoordinate)
		{
			if (Nodes.IsEmpty()
				|| Nodes[0].DeadlineSeconds > CurrentTimeSeconds)
			{
				return false;
			}

			OutChunkCoordinate = Nodes[0].ChunkCoordinate;
			RemoveAt(0);
			return true;
		}

		bool IsEmpty() const { return Nodes.IsEmpty(); }
		SIZE_T GetAllocatedSize() const
		{
			return Nodes.GetAllocatedSize() + IndexByChunk.GetAllocatedSize();
		}

		void Reset()
		{
			Nodes.Reset();
			IndexByChunk.Reset();
		}

	private:
		bool IsLess(const int32 Left, const int32 Right) const
		{
			const FBuildSpatialIdleDeadline& LeftNode = Nodes[Left];
			const FBuildSpatialIdleDeadline& RightNode = Nodes[Right];
			if (LeftNode.DeadlineSeconds != RightNode.DeadlineSeconds)
			{
				return LeftNode.DeadlineSeconds < RightNode.DeadlineSeconds;
			}
			if (LeftNode.ChunkCoordinate.X != RightNode.ChunkCoordinate.X)
			{
				return LeftNode.ChunkCoordinate.X < RightNode.ChunkCoordinate.X;
			}
			if (LeftNode.ChunkCoordinate.Y != RightNode.ChunkCoordinate.Y)
			{
				return LeftNode.ChunkCoordinate.Y < RightNode.ChunkCoordinate.Y;
			}
			return LeftNode.ChunkCoordinate.Z < RightNode.ChunkCoordinate.Z;
		}

		void SwapNodes(const int32 Left, const int32 Right)
		{
			if (Left == Right)
			{
				return;
			}
			Nodes.Swap(Left, Right);
			IndexByChunk.FindChecked(Nodes[Left].ChunkCoordinate) = Left;
			IndexByChunk.FindChecked(Nodes[Right].ChunkCoordinate) = Right;
		}

		void SiftUp(int32 Index)
		{
			while (Index > 0)
			{
				const int32 ParentIndex = (Index - 1) / 2;
				if (!IsLess(Index, ParentIndex))
				{
					break;
				}
				SwapNodes(Index, ParentIndex);
				Index = ParentIndex;
			}
		}

		void SiftDown(int32 Index)
		{
			while (true)
			{
				const int32 LeftChild = Index * 2 + 1;
				if (!Nodes.IsValidIndex(LeftChild))
				{
					break;
				}
				const int32 RightChild = LeftChild + 1;
				int32 SmallestChild = LeftChild;
				if (Nodes.IsValidIndex(RightChild)
					&& IsLess(RightChild, LeftChild))
				{
					SmallestChild = RightChild;
				}
				if (!IsLess(SmallestChild, Index))
				{
					break;
				}
				SwapNodes(Index, SmallestChild);
				Index = SmallestChild;
			}
		}

		void RemoveAt(const int32 Index)
		{
			check(Nodes.IsValidIndex(Index));
			IndexByChunk.Remove(Nodes[Index].ChunkCoordinate);
			const int32 LastIndex = Nodes.Num() - 1;
			if (Index == LastIndex)
			{
				Nodes.Pop(EAllowShrinking::No);
				return;
			}

			Nodes[Index] = MoveTemp(Nodes[LastIndex]);
			Nodes.Pop(EAllowShrinking::No);
			IndexByChunk.FindChecked(Nodes[Index].ChunkCoordinate) = Index;
			if (Index > 0 && IsLess(Index, (Index - 1) / 2))
			{
				SiftUp(Index);
			}
			else
			{
				SiftDown(Index);
			}
		}

		TArray<FBuildSpatialIdleDeadline> Nodes;
		TMap<FIntVector, int32> IndexByChunk;
	};

	bool IsValidSpatialMobility(const EBuildSpatialMobility Mobility)
	{
		return Mobility == EBuildSpatialMobility::Static
			|| Mobility == EBuildSpatialMobility::Dynamic;
	}
}

class FBuildSpatialIndexData final
{
public:
	explicit FBuildSpatialIndexData(const FBuildSpatialIndexConfig& InConfig)
		: Config(InConfig)
		, AsyncState(MakeShared<FBuildSpatialAsyncState, ESPMode::ThreadSafe>())
	{
	}

	~FBuildSpatialIndexData()
	{
		DeactivateAsyncState();
	}

		bool TryGetChunkRange(
		const FBox& Bounds,
		FIntVector& OutMinChunk,
		FIntVector& OutMaxChunk,
		int64& OutChunkCount) const
	{
		if (!IsValidSpatialBounds(Bounds))
		{
			return false;
		}

		if (!Config.TryGetChunkCoordinate(Bounds.Min, OutMinChunk)
			|| !Config.TryGetChunkCoordinate(Bounds.Max, OutMaxChunk))
		{
			return false;
		}

		const int64 CountX = static_cast<int64>(OutMaxChunk.X) - OutMinChunk.X + 1;
		const int64 CountY = static_cast<int64>(OutMaxChunk.Y) - OutMinChunk.Y + 1;
		const int64 CountZ = static_cast<int64>(OutMaxChunk.Z) - OutMinChunk.Z + 1;
		if (CountX <= 0 || CountY <= 0 || CountZ <= 0
			|| CountX > MAX_int64 / CountY
			|| CountX * CountY > MAX_int64 / CountZ)
		{
			return false;
		}

			OutChunkCount = CountX * CountY * CountZ;
			return true;
		}

	template <typename CallbackType>
	static void ForEachChunkCoordinate(
		const FIntVector& MinChunk,
		const FIntVector& MaxChunk,
		CallbackType&& Callback)
	{
		for (int64 X = MinChunk.X; X <= static_cast<int64>(MaxChunk.X); ++X)
		{
			for (int64 Y = MinChunk.Y; Y <= static_cast<int64>(MaxChunk.Y); ++Y)
			{
				for (int64 Z = MinChunk.Z; Z <= static_cast<int64>(MaxChunk.Z); ++Z)
				{
					Callback(FIntVector(
						static_cast<int32>(X),
						static_cast<int32>(Y),
						static_cast<int32>(Z)));
				}
			}
		}
	}

	FBuildSpatialEntityRecord* FindEntity(const FBuildEntityHandle Entity)
	{
		if (!Entity.IsSet() || !EntitiesBySlot.IsValidIndex(Entity.GetIndex()))
		{
			return nullptr;
		}

		FBuildSpatialEntityRecord& Record = EntitiesBySlot[Entity.GetIndex()];
		return Record.Entity == Entity ? &Record : nullptr;
	}

	const FBuildSpatialEntityRecord* FindEntity(const FBuildEntityHandle Entity) const
	{
		return const_cast<FBuildSpatialIndexData*>(this)->FindEntity(Entity);
	}

	FBuildSpatialEntityRecord& AddEntitySlot(const FBuildEntityHandle Entity)
	{
		check(Entity.IsSet());
		if (!EntitiesBySlot.IsValidIndex(Entity.GetIndex()))
		{
			EntitiesBySlot.SetNum(Entity.GetIndex() + 1, EAllowShrinking::No);
		}

		FBuildSpatialEntityRecord& Record = EntitiesBySlot[Entity.GetIndex()];
		check(!Record.Entity.IsSet());
		Record.Entity = Entity;
		++EntityCount;
		return Record;
	}

		void RemoveEntitySlot(const FBuildEntityHandle Entity)
		{
		FBuildSpatialEntityRecord* Record = FindEntity(Entity);
		check(Record);
		*Record = FBuildSpatialEntityRecord();
		--EntityCount;
			check(EntityCount >= 0);
		}

	bool IsChunkSnapshotSchedulable(const FBuildSpatialChunk& Chunk) const
	{
		return Chunk.GetStaticCount() >= Config.AsyncSnapshotMinimumStaticEntries
			&& Chunk.IsStaticDirty();
	}

	bool IsChunkSnapshotEligible(
		const FBuildSpatialChunk& Chunk,
		const FBuildSpatialChunkAsyncState& ChunkState,
		const double CurrentTimeSeconds) const
	{
		if (!IsChunkSnapshotSchedulable(Chunk) || ChunkState.bInFlight)
		{
			return false;
		}

		const int32 DeltaThreshold = FMath::Max(
			Config.AsyncSnapshotMinimumDeltaEntries,
			FMath::CeilToInt(
				Chunk.GetStaticSnapshotCount() * Config.AsyncSnapshotDeltaRatio));
		const int32 TombstoneThreshold = FMath::Max(
			Config.AsyncSnapshotMinimumTombstones,
			FMath::CeilToInt(
				Chunk.GetStaticSnapshotCount() * Config.AsyncSnapshotTombstoneRatio));
		const bool bIdle = CurrentTimeSeconds - Chunk.GetLastStaticWriteTimeSeconds()
			>= Config.AsyncSnapshotIdleSeconds;
		if (ChunkState.StaleRetryDeltaThreshold > 0)
		{
			return bIdle
				|| Chunk.GetStaticDeltaCount()
					>= ChunkState.StaleRetryDeltaThreshold;
		}

		return bIdle
			|| Chunk.GetStaticDeltaCount() >= DeltaThreshold
			|| Chunk.GetStaticTombstoneCount() >= TombstoneThreshold;
	}

	bool IsChunkSnapshotImmediatelyEligible(
		const FBuildSpatialChunk& Chunk,
		const FBuildSpatialChunkAsyncState& ChunkState) const
	{
		if (!IsChunkSnapshotSchedulable(Chunk) || ChunkState.bInFlight)
		{
			return false;
		}

		if (ChunkState.StaleRetryDeltaThreshold > 0)
		{
			return Chunk.GetStaticDeltaCount()
				>= ChunkState.StaleRetryDeltaThreshold;
		}

		const int32 DeltaThreshold = FMath::Max(
			Config.AsyncSnapshotMinimumDeltaEntries,
			FMath::CeilToInt(
				Chunk.GetStaticSnapshotCount() * Config.AsyncSnapshotDeltaRatio));
		const int32 TombstoneThreshold = FMath::Max(
			Config.AsyncSnapshotMinimumTombstones,
			FMath::CeilToInt(
				Chunk.GetStaticSnapshotCount() * Config.AsyncSnapshotTombstoneRatio));
		return Chunk.GetStaticDeltaCount() >= DeltaThreshold
			|| Chunk.GetStaticTombstoneCount() >= TombstoneThreshold;
	}

	void CancelReady(
		const FIntVector& ChunkCoordinate,
		FBuildSpatialChunkAsyncState& ChunkState)
	{
		if (!ChunkState.bReadyQueued)
		{
			return;
		}
		ChunkState.bReadyQueued = false;
		--ReadyChunkCount;
		check(ReadyChunkCount >= 0);
		if (ReadyChunkCount == 0)
		{
			ReadyChunks.Reset();
			ReadyQueueIndexByChunk.Reset();
			ReadyQueueHead = 0;
		}
	}

	void EnqueueReady(
		const FIntVector& ChunkCoordinate,
		FBuildSpatialChunkAsyncState& ChunkState)
	{
		IdleDeadlines.Remove(ChunkCoordinate);
		if (ChunkState.bReadyQueued)
		{
			return;
		}

		ChunkState.bReadyQueued = true;
		++ReadyChunkCount;
		if (!ReadyQueueIndexByChunk.Contains(ChunkCoordinate))
		{
			const int32 QueueIndex = ReadyChunks.Add(ChunkCoordinate);
			ReadyQueueIndexByChunk.Add(ChunkCoordinate, QueueIndex);
		}
	}

	void RefreshSnapshotScheduling(
		const FIntVector& ChunkCoordinate,
		const FBuildSpatialChunk& Chunk)
	{
		if (!Chunk.IsStaticDirty())
		{
			DirtyStaticChunks.Remove(ChunkCoordinate);
			IdleDeadlines.Remove(ChunkCoordinate);
			if (FBuildSpatialChunkAsyncState* State =
				ChunkAsyncStates.Find(ChunkCoordinate))
			{
				CancelReady(ChunkCoordinate, *State);
				State->StaleRetryDeltaThreshold = 0;
				if (!State->bInFlight)
				{
					ChunkAsyncStates.Remove(ChunkCoordinate);
				}
			}
			return;
		}

		DirtyStaticChunks.Add(ChunkCoordinate);
		FBuildSpatialChunkAsyncState& State =
			ChunkAsyncStates.FindOrAdd(ChunkCoordinate);
		if (State.bInFlight || !IsChunkSnapshotSchedulable(Chunk))
		{
			CancelReady(ChunkCoordinate, State);
			IdleDeadlines.Remove(ChunkCoordinate);
			return;
		}

		if (IsChunkSnapshotImmediatelyEligible(Chunk, State))
		{
			EnqueueReady(ChunkCoordinate, State);
			return;
		}

		CancelReady(ChunkCoordinate, State);
		IdleDeadlines.AddOrUpdate(
			ChunkCoordinate,
			Chunk.GetLastStaticWriteTimeSeconds()
				+ Config.AsyncSnapshotIdleSeconds);
	}

	void RetireChunkScheduling(const FIntVector& ChunkCoordinate)
	{
		DirtyStaticChunks.Remove(ChunkCoordinate);
		IdleDeadlines.Remove(ChunkCoordinate);
		if (FBuildSpatialChunkAsyncState* State =
			ChunkAsyncStates.Find(ChunkCoordinate))
		{
			CancelReady(ChunkCoordinate, *State);
			if (!State->bInFlight)
			{
				ChunkAsyncStates.Remove(ChunkCoordinate);
			}
		}
	}

	bool PopScheduleNode(
		const double CurrentTimeSeconds,
		FIntVector& OutChunkCoordinate,
		bool& bOutActiveCandidate)
	{
		bOutActiveCandidate = false;
		if (ReadyQueueHead < ReadyChunks.Num())
		{
			const int32 QueueIndex = ReadyQueueHead++;
			const FIntVector ChunkCoordinate = ReadyChunks[QueueIndex];
			const int32* StoredQueueIndex =
				ReadyQueueIndexByChunk.Find(ChunkCoordinate);
			if (StoredQueueIndex && *StoredQueueIndex == QueueIndex)
			{
				ReadyQueueIndexByChunk.Remove(ChunkCoordinate);
				FBuildSpatialChunkAsyncState* State =
					ChunkAsyncStates.Find(ChunkCoordinate);
				if (State && State->bReadyQueued)
				{
					State->bReadyQueued = false;
					--ReadyChunkCount;
					check(ReadyChunkCount >= 0);
					OutChunkCoordinate = ChunkCoordinate;
					bOutActiveCandidate = true;
				}
			}
			CompactReadyQueueIfDrained();
			return true;
		}

		CompactReadyQueueIfDrained();
		bOutActiveCandidate =
			IdleDeadlines.PopDue(CurrentTimeSeconds, OutChunkCoordinate);
		return bOutActiveCandidate;
	}

	void CompactReadyQueueIfDrained()
	{
		if (ReadyQueueHead < ReadyChunks.Num())
		{
			return;
		}
		ReadyChunks.Reset();
		ReadyQueueIndexByChunk.Reset();
		ReadyQueueHead = 0;
	}

	void DeactivateAsyncState()
	{
		if (AsyncState)
		{
			AsyncState->bActive.Store(false);
		}
		AsyncState.Reset();
		ChunkAsyncStates.Reset();
		DirtyStaticChunks.Reset();
		ReadyChunks.Reset();
		ReadyQueueIndexByChunk.Reset();
		ReadyQueueHead = 0;
		ReadyChunkCount = 0;
		IdleDeadlines.Reset();
	}

		void ResetAsyncState()
		{
			DeactivateAsyncState();
			AsyncState = MakeShared<FBuildSpatialAsyncState, ESPMode::ThreadSafe>();
		}

	void BumpQueryRevision()
	{
		QueryRevision = QueryRevision == MAX_uint64 ? 1 : QueryRevision + 1;
	}

	FBuildSpatialIndexConfig Config;
	TArray<FBuildSpatialEntityRecord, FBuildStableArrayAllocator> EntitiesBySlot;
		int32 EntityCount = 0;
		uint64 QueryRevision = 1;
		TMap<FIntVector, TUniquePtr<FBuildSpatialChunk>> Chunks;
		TSharedPtr<FBuildSpatialAsyncState, ESPMode::ThreadSafe> AsyncState;
		TMap<FIntVector, FBuildSpatialChunkAsyncState> ChunkAsyncStates;
	TSet<FIntVector> DirtyStaticChunks;
	TArray<FIntVector> ReadyChunks;
	TMap<FIntVector, int32> ReadyQueueIndexByChunk;
	int32 ReadyQueueHead = 0;
	int32 ReadyChunkCount = 0;
	FBuildSpatialIdleDeadlineHeap IdleDeadlines;
};

bool FBuildSpatialIndexConfig::IsValid() const
{
	return FMath::IsFinite(ChunkSize)
			&& ChunkSize > 0.0
		&& FMath::IsFinite(DynamicBoundsPadding)
		&& DynamicBoundsPadding >= 0.0
		&& MaxChunksPerEntity > 0
		&& AsyncSnapshotMinimumStaticEntries > 0
		&& AsyncSnapshotMinimumDeltaEntries > 0
		&& FMath::IsFinite(AsyncSnapshotDeltaRatio)
		&& AsyncSnapshotDeltaRatio >= 0.0
		&& AsyncSnapshotMinimumTombstones > 0
		&& FMath::IsFinite(AsyncSnapshotTombstoneRatio)
		&& AsyncSnapshotTombstoneRatio >= 0.0
		&& FMath::IsFinite(AsyncSnapshotIdleSeconds)
		&& AsyncSnapshotIdleSeconds >= 0.0
		&& AsyncSnapshotMaxCapturesPerTick > 0
		&& AsyncSnapshotMaxPublishesPerTick > 0
		&& AsyncSnapshotMaxConcurrentBuilds > 0
		&& AsyncSnapshotMaxScheduleCandidatesPerTick > 0;
}

bool FBuildSpatialIndexConfig::TryGetChunkCoordinate(
	const FVector& WorldLocation,
	FIntVector& OutChunkCoordinate) const
{
	if (!IsValid()
		|| !FMath::IsFinite(WorldLocation.X)
		|| !FMath::IsFinite(WorldLocation.Y)
		|| !FMath::IsFinite(WorldLocation.Z))
	{
		return false;
	}

	const auto ToChunkCoordinate = [this](const double Coordinate, int32& OutCoordinate)
	{
		const double ChunkCoordinate = FMath::Floor(Coordinate / ChunkSize);
		if (ChunkCoordinate < static_cast<double>(MIN_int32)
			|| ChunkCoordinate > static_cast<double>(MAX_int32))
		{
			return false;
		}

		OutCoordinate = static_cast<int32>(ChunkCoordinate);
		return true;
	};

	FIntVector ChunkCoordinate;
	if (!ToChunkCoordinate(WorldLocation.X, ChunkCoordinate.X)
		|| !ToChunkCoordinate(WorldLocation.Y, ChunkCoordinate.Y)
		|| !ToChunkCoordinate(WorldLocation.Z, ChunkCoordinate.Z))
	{
		return false;
	}

	OutChunkCoordinate = ChunkCoordinate;
	return true;
}

FBuildSpatialIndex::FBuildSpatialIndex(const FBuildSpatialIndexConfig& InConfig)
{
	check(IsInGameThread());
	checkf(InConfig.IsValid(), TEXT("Invalid Building spatial index config."));
	Data = MakeUnique<FBuildSpatialIndexData>(InConfig);
}

FBuildSpatialIndex::~FBuildSpatialIndex() = default;

bool FBuildSpatialIndex::Insert(
	const FBuildEntityHandle Entity,
	const FBox& Bounds,
	const EBuildSpatialMobility Mobility)
{
	check(IsInGameThread());
	if (!Entity.IsSet()
		|| !IsValidSpatialMobility(Mobility)
		|| Data->FindEntity(Entity))
	{
		return false;
	}

	FIntVector MinChunk;
	FIntVector MaxChunk;
	int64 ChunkCount = 0;
	if (!Data->TryGetChunkRange(Bounds, MinChunk, MaxChunk, ChunkCount)
		|| ChunkCount > Data->Config.MaxChunksPerEntity)
	{
		return false;
	}

	FBuildSpatialEntityRecord Record;
	Record.Entity = Entity;
	Record.Bounds = Bounds;
	Record.Mobility = Mobility;
	Record.Chunks.Reserve(static_cast<int32>(ChunkCount));
	FBuildSpatialIndexData::ForEachChunkCoordinate(
		MinChunk,
		MaxChunk,
		[this, Entity, &Bounds, Mobility, &Record](const FIntVector& ChunkCoordinate)
		{
			TUniquePtr<FBuildSpatialChunk>& Chunk = Data->Chunks.FindOrAdd(ChunkCoordinate);
			if (!Chunk)
			{
				Chunk = MakeUnique<FBuildSpatialChunk>(Data->Config.DynamicBoundsPadding);
			}
			verify(Chunk->Insert(Entity, Bounds, Mobility));
			if (Mobility == EBuildSpatialMobility::Static)
			{
				Data->RefreshSnapshotScheduling(ChunkCoordinate, *Chunk);
			}
			Record.Chunks.Add(ChunkCoordinate);
		});

	FBuildSpatialEntityRecord& StoredRecord = Data->AddEntitySlot(Entity);
	StoredRecord = MoveTemp(Record);
	Data->BumpQueryRevision();
	return true;
}

void FBuildSpatialIndex::ReserveEntityCapacity(const int32 EntityCapacity)
{
	check(IsInGameThread());
	if (EntityCapacity > 0)
	{
		Data->EntitiesBySlot.Reserve(EntityCapacity);
	}
}

bool FBuildSpatialIndex::Update(
	const FBuildEntityHandle Entity,
	const FBox& Bounds)
{
	check(IsInGameThread());
	FBuildSpatialEntityRecord* ExistingRecord = Data->FindEntity(Entity);
	if (!ExistingRecord || !IsValidSpatialBounds(Bounds))
	{
		return false;
	}

	if (ExistingRecord->Bounds == Bounds)
	{
		return true;
	}

	FIntVector MinChunk;
	FIntVector MaxChunk;
	int64 ChunkCount = 0;
	if (!Data->TryGetChunkRange(Bounds, MinChunk, MaxChunk, ChunkCount)
		|| ChunkCount > Data->Config.MaxChunksPerEntity)
	{
		return false;
	}

	TArray<FIntVector, TInlineAllocator<4>> NewChunks;
	NewChunks.Reserve(static_cast<int32>(ChunkCount));
	FBuildSpatialIndexData::ForEachChunkCoordinate(
		MinChunk,
		MaxChunk,
		[&NewChunks](const FIntVector& ChunkCoordinate)
		{
			NewChunks.Add(ChunkCoordinate);
		});

	if (ExistingRecord->Chunks == NewChunks)
	{
		for (const FIntVector& ChunkCoordinate : ExistingRecord->Chunks)
		{
			TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
			check(Chunk && *Chunk);
			verify((*Chunk)->Update(Entity, Bounds, ExistingRecord->Mobility));
			if (ExistingRecord->Mobility == EBuildSpatialMobility::Static)
			{
				Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
			}
		}
		ExistingRecord->Bounds = Bounds;
		Data->BumpQueryRevision();
		return true;
	}

	TArray<FIntVector, TInlineAllocator<4>> OldChunks = MoveTemp(ExistingRecord->Chunks);
	for (const FIntVector& ChunkCoordinate : OldChunks)
	{
		TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
		check(Chunk && *Chunk);
		verify((*Chunk)->Remove(Entity, ExistingRecord->Mobility));
		if (ExistingRecord->Mobility == EBuildSpatialMobility::Static)
		{
			Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
		}
		if ((*Chunk)->IsEmpty())
		{
			const FBuildSpatialChunkAsyncState* AsyncChunkState =
				Data->ChunkAsyncStates.Find(ChunkCoordinate);
			if (!AsyncChunkState || !AsyncChunkState->bInFlight)
			{
				Data->RetireChunkScheduling(ChunkCoordinate);
				Data->Chunks.Remove(ChunkCoordinate);
			}
		}
	}

	ExistingRecord->Bounds = Bounds;
	ExistingRecord->Chunks = MoveTemp(NewChunks);
	for (const FIntVector& ChunkCoordinate : ExistingRecord->Chunks)
	{
		TUniquePtr<FBuildSpatialChunk>& Chunk = Data->Chunks.FindOrAdd(ChunkCoordinate);
		if (!Chunk)
		{
			Chunk = MakeUnique<FBuildSpatialChunk>(Data->Config.DynamicBoundsPadding);
		}
		verify(Chunk->Insert(Entity, Bounds, ExistingRecord->Mobility));
		if (ExistingRecord->Mobility == EBuildSpatialMobility::Static)
		{
			Data->RefreshSnapshotScheduling(ChunkCoordinate, *Chunk);
		}
	}
	Data->BumpQueryRevision();
	return true;
}

bool FBuildSpatialIndex::SetMobility(
	const FBuildEntityHandle Entity,
	const EBuildSpatialMobility Mobility)
{
	check(IsInGameThread());
	FBuildSpatialEntityRecord* Record = Data->FindEntity(Entity);
	if (!Record || !IsValidSpatialMobility(Mobility))
	{
		return false;
	}

	if (Record->Mobility == Mobility)
	{
		return true;
	}

	for (const FIntVector& ChunkCoordinate : Record->Chunks)
	{
		TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
		check(Chunk && *Chunk);
		verify((*Chunk)->SetMobility(Entity, Mobility));
		Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
	}

	Record->Mobility = Mobility;
	Data->BumpQueryRevision();
	return true;
}

bool FBuildSpatialIndex::Remove(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	FBuildSpatialEntityRecord* Record = Data->FindEntity(Entity);
	if (!Record)
	{
		return false;
	}

	for (const FIntVector& ChunkCoordinate : Record->Chunks)
	{
		TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
		check(Chunk && *Chunk);
		verify((*Chunk)->Remove(Entity, Record->Mobility));
		if (Record->Mobility == EBuildSpatialMobility::Static)
		{
			Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
		}
		if ((*Chunk)->IsEmpty())
		{
			const FBuildSpatialChunkAsyncState* AsyncChunkState =
				Data->ChunkAsyncStates.Find(ChunkCoordinate);
			if (!AsyncChunkState || !AsyncChunkState->bInFlight)
			{
				Data->RetireChunkScheduling(ChunkCoordinate);
				Data->Chunks.Remove(ChunkCoordinate);
			}
		}
	}

	Data->RemoveEntitySlot(Entity);
	Data->BumpQueryRevision();
	return true;
}

bool FBuildSpatialIndex::Contains(const FBuildEntityHandle Entity) const
{
	check(IsInGameThread());
	return Data->FindEntity(Entity) != nullptr;
}

bool FBuildSpatialIndex::TryGetBounds(
	const FBuildEntityHandle Entity,
	FBox& OutBounds) const
{
	check(IsInGameThread());
	const FBuildSpatialEntityRecord* Record = Data->FindEntity(Entity);
	if (!Record)
	{
		return false;
	}

	OutBounds = Record->Bounds;
	return true;
}

bool FBuildSpatialIndex::TryGetMobility(
	const FBuildEntityHandle Entity,
	EBuildSpatialMobility& OutMobility) const
{
	check(IsInGameThread());
	const FBuildSpatialEntityRecord* Record = Data->FindEntity(Entity);
	if (!Record)
	{
		return false;
	}

	OutMobility = Record->Mobility;
	return true;
}

void FBuildSpatialIndex::QueryOverlaps(
	const FBox& QueryBounds,
	FBuildSpatialQueryScratch& Scratch,
	TArray<FBuildEntityHandle>& OutEntities) const
{
	check(IsInGameThread());
	OutEntities.Reset();
	Scratch.Candidates.Reset();
	Scratch.UniqueEntities.Reset();
	if (Data->Chunks.IsEmpty() || !IsValidSpatialBounds(QueryBounds))
	{
		return;
	}

	if (!Data->Chunks.IsEmpty())
	{
		FIntVector MinChunk;
		FIntVector MaxChunk;
		int64 QueryChunkCount = 0;
		if (!Data->TryGetChunkRange(QueryBounds, MinChunk, MaxChunk, QueryChunkCount))
		{
			return;
		}
		const int64 SparseScanThreshold = FMath::Max<int64>(64, Data->Chunks.Num() * 2ll);
		if (QueryChunkCount <= SparseScanThreshold)
		{
			FBuildSpatialIndexData::ForEachChunkCoordinate(
				MinChunk,
				MaxChunk,
				[this, &QueryBounds, &Scratch](const FIntVector& ChunkCoordinate)
				{
					const TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
					if (Chunk && *Chunk)
					{
						(*Chunk)->Query(QueryBounds, Scratch.Candidates);
					}
				});
		}
		else
		{
			// 巨型 Query 不枚举海量空 Cell，而是扫描实际存在的 Sparse Chunk。
			for (const TPair<FIntVector, TUniquePtr<FBuildSpatialChunk>>& Pair : Data->Chunks)
			{
				const FIntVector& Coordinate = Pair.Key;
				if (Coordinate.X >= MinChunk.X && Coordinate.X <= MaxChunk.X
					&& Coordinate.Y >= MinChunk.Y && Coordinate.Y <= MaxChunk.Y
					&& Coordinate.Z >= MinChunk.Z && Coordinate.Z <= MaxChunk.Z)
				{
					Pair.Value->Query(QueryBounds, Scratch.Candidates);
				}
			}
		}
	}

	Scratch.UniqueEntities.Reserve(Scratch.Candidates.Num());
	OutEntities.Reserve(Scratch.Candidates.Num());
	for (const FBuildEntityHandle Candidate : Scratch.Candidates)
	{
		if (Scratch.UniqueEntities.Contains(Candidate))
		{
			continue;
		}

		const FBuildSpatialEntityRecord* Record = Data->FindEntity(Candidate);
		if (Record && Record->Bounds.Intersect(QueryBounds))
		{
			Scratch.UniqueEntities.Add(Candidate);
			OutEntities.Add(Candidate);
		}
	}
}

void FBuildSpatialIndex::QueryRay(
	const FVector& Origin,
	const FVector& Direction,
	const double MaxDistance,
	FBuildSpatialQueryScratch& Scratch,
	TArray<FBuildSpatialRayHit>& OutHits) const
{
	check(IsInGameThread());
	OutHits.Reset();
	Scratch.Candidates.Reset();
	Scratch.UniqueEntities.Reset();
	if (Data->Chunks.IsEmpty()
		|| !IsFiniteSpatialVector(Origin)
		|| !IsFiniteSpatialVector(Direction)
		|| Direction.IsNearlyZero()
		|| !FMath::IsFinite(MaxDistance)
		|| MaxDistance < 0.0)
	{
		return;
	}

	const FVector UnitDirection = Direction.GetSafeNormal();
	const FVector RayEnd = Origin + UnitDirection * MaxDistance;
	if (!IsFiniteSpatialVector(RayEnd))
	{
		return;
	}

	FIntVector CurrentChunk;
	FIntVector EndChunk;
	const auto QueryChunk = [this, &Origin, &UnitDirection, MaxDistance, &Scratch](
		const FIntVector& ChunkCoordinate)
	{
		const TUniquePtr<FBuildSpatialChunk>* Chunk = Data->Chunks.Find(ChunkCoordinate);
		if (Chunk && *Chunk)
	{
			(*Chunk)->QueryRay(Origin, UnitDirection, MaxDistance, Scratch.Candidates);
		}
	};

	if (!Data->Chunks.IsEmpty())
	{
		if (!Data->Config.TryGetChunkCoordinate(Origin, CurrentChunk)
			|| !Data->Config.TryGetChunkCoordinate(RayEnd, EndChunk))
		{
			return;
		}
		QueryChunk(CurrentChunk);
	}
	if (!Data->Chunks.IsEmpty() && CurrentChunk != EndChunk)
	{
		FIntVector Step = FIntVector::ZeroValue;
		FVector NextBoundaryDistance(
			TNumericLimits<double>::Max(),
			TNumericLimits<double>::Max(),
			TNumericLimits<double>::Max());
		FVector ChunkCrossingDistance(
			TNumericLimits<double>::Max(),
			TNumericLimits<double>::Max(),
			TNumericLimits<double>::Max());

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double AxisDirection = UnitDirection[Axis];
			if (FMath::IsNearlyZero(AxisDirection, 1.0e-12))
			{
				continue;
			}

			Step[Axis] = AxisDirection > 0.0 ? 1 : -1;
			const double NextBoundary = Step[Axis] > 0
				? (static_cast<double>(CurrentChunk[Axis]) + 1.0) * Data->Config.ChunkSize
				: static_cast<double>(CurrentChunk[Axis]) * Data->Config.ChunkSize;
			NextBoundaryDistance[Axis] = (NextBoundary - Origin[Axis]) / AxisDirection;
			ChunkCrossingDistance[Axis] = Data->Config.ChunkSize / FMath::Abs(AxisDirection);
		}

		const int64 MaxTraversalSteps =
			FMath::Abs(static_cast<int64>(EndChunk.X) - CurrentChunk.X)
			+ FMath::Abs(static_cast<int64>(EndChunk.Y) - CurrentChunk.Y)
			+ FMath::Abs(static_cast<int64>(EndChunk.Z) - CurrentChunk.Z)
			+ 3;
		for (int64 TraversalStep = 0;
			CurrentChunk != EndChunk && TraversalStep < MaxTraversalSteps;
			++TraversalStep)
		{
			int32 Axis = 0;
			if (NextBoundaryDistance.Y < NextBoundaryDistance.X)
			{
				Axis = 1;
			}
			if (NextBoundaryDistance.Z < NextBoundaryDistance[Axis])
			{
				Axis = 2;
			}

			if (NextBoundaryDistance[Axis] > MaxDistance + UE_DOUBLE_SMALL_NUMBER
				|| Step[Axis] == 0)
			{
				break;
			}

			CurrentChunk[Axis] += Step[Axis];
			NextBoundaryDistance[Axis] += ChunkCrossingDistance[Axis];
			QueryChunk(CurrentChunk);
		}
	}

	Scratch.UniqueEntities.Reserve(Scratch.Candidates.Num());
	OutHits.Reserve(Scratch.Candidates.Num());
	for (const FBuildEntityHandle Candidate : Scratch.Candidates)
	{
		if (Scratch.UniqueEntities.Contains(Candidate))
		{
			continue;
		}

		const FBuildSpatialEntityRecord* Record = Data->FindEntity(Candidate);
		double HitDistance = 0.0;
		if (Record && RaycastBounds(
			Record->Bounds,
			Origin,
			UnitDirection,
			MaxDistance,
			HitDistance))
		{
			Scratch.UniqueEntities.Add(Candidate);
			OutHits.Add({Candidate, HitDistance});
		}
	}

	OutHits.Sort(
		[](const FBuildSpatialRayHit& Left, const FBuildSpatialRayHit& Right)
		{
			if (Left.Distance != Right.Distance)
			{
				return Left.Distance < Right.Distance;
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
		});
}

int32 FBuildSpatialIndex::GetEntityCount() const
{
	check(IsInGameThread());
	return Data->EntityCount;
}

uint64 FBuildSpatialIndex::GetQueryRevision() const
{
	check(IsInGameThread());
	return Data->QueryRevision;
}

int32 FBuildSpatialIndex::GetChunkCount() const
{
	check(IsInGameThread());
	return Data->Chunks.Num();
}

int32 FBuildSpatialIndex::GetDirtyStaticChunkCount() const
{
	check(IsInGameThread());
	return Data->DirtyStaticChunks.Num();
}

SIZE_T FBuildSpatialIndex::GetEstimatedAllocatedSize() const
{
	check(IsInGameThread());
	SIZE_T AllocatedSize = Data->EntitiesBySlot.GetAllocatedSize()
		+ Data->Chunks.GetAllocatedSize()
		+ Data->ChunkAsyncStates.GetAllocatedSize()
		+ Data->DirtyStaticChunks.GetAllocatedSize()
		+ Data->ReadyChunks.GetAllocatedSize()
		+ Data->ReadyQueueIndexByChunk.GetAllocatedSize()
		+ Data->IdleDeadlines.GetAllocatedSize();
	for (const FBuildSpatialEntityRecord& Record : Data->EntitiesBySlot)
	{
		AllocatedSize += Record.Chunks.GetAllocatedSize();
	}
	for (const TPair<FIntVector, TUniquePtr<FBuildSpatialChunk>>& Pair : Data->Chunks)
	{
		AllocatedSize += Pair.Value ? Pair.Value->GetAllocatedSize() : 0;
	}
	return AllocatedSize;
}

int32 FBuildSpatialIndex::GetAsyncSnapshotInFlightCount() const
{
	check(IsInGameThread());
	return Data->AsyncState ? Data->AsyncState->InFlightCount.Load() : 0;
}

bool FBuildSpatialIndex::HasPendingAsyncSnapshotWork() const
{
	check(IsInGameThread());
	if (!Data->AsyncState)
	{
		return false;
	}
	if (Data->AsyncState->InFlightCount.Load() > 0
		|| !Data->AsyncState->CompletedResults.IsEmpty())
	{
		return true;
	}
	return Data->ReadyChunkCount > 0 || !Data->IdleDeadlines.IsEmpty();
}

FBuildSpatialSnapshotWorkStats FBuildSpatialIndex::ProcessAsyncSnapshotWork()
{
	check(IsInGameThread());
	FBuildSpatialSnapshotWorkStats Stats;
	if (!Data->AsyncState || !Data->AsyncState->bActive.Load())
	{
		return Stats;
	}

	const double CurrentTimeSeconds = FPlatformTime::Seconds();
	const auto TryPublishChunk = [this, &Stats]()
	{
		TUniquePtr<FBuildSpatialSnapshotResult> Result;
		if (!Data->AsyncState->CompletedResults.Dequeue(Result) || !Result)
		{
			return false;
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Snapshot_Publish);
		CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, SnapshotPublish);
		FBuildSpatialChunkAsyncState* ChunkAsyncState =
			Data->ChunkAsyncStates.Find(Result->ChunkCoordinate);
		if (ChunkAsyncState)
		{
			ChunkAsyncState->bInFlight = false;
		}

		TUniquePtr<FBuildSpatialChunk>* Chunk =
			Data->Chunks.Find(Result->ChunkCoordinate);
		if (Chunk
			&& *Chunk
			&& (*Chunk)->PublishStaticSnapshot(
				Result->StaticVersion,
				MoveTemp(Result->Snapshot)))
		{
			++Stats.PublishedChunks;
			Data->RefreshSnapshotScheduling(Result->ChunkCoordinate, **Chunk);
			return true;
		}

		++Stats.DiscardedStaleChunks;
		if (!Chunk || !*Chunk)
		{
			Data->RetireChunkScheduling(Result->ChunkCoordinate);
			return true;
		}
		if ((*Chunk)->GetStaticCount() == 0)
		{
			// A Chunk with no Static leaves remains alive while its worker is in
			// flight so a remove/recreate cycle cannot reuse the captured uint64
			// version. Once the stale result returns its async record can retire;
			// the Chunk itself remains only when it still owns Dynamic leaves.
			if ((*Chunk)->IsEmpty())
			{
				Data->Chunks.Remove(Result->ChunkCoordinate);
			}
			Data->RetireChunkScheduling(Result->ChunkCoordinate);
			return true;
		}

		FBuildSpatialChunkAsyncState& RetryState =
			Data->ChunkAsyncStates.FindOrAdd(Result->ChunkCoordinate);
		RetryState.bInFlight = false;
		const int64 DoubledDelta =
			static_cast<int64>((*Chunk)->GetStaticDeltaCount()) * 2ll;
		RetryState.StaleRetryDeltaThreshold = static_cast<int32>(FMath::Clamp<int64>(
			DoubledDelta,
			1,
			MAX_int32));
		Data->RefreshSnapshotScheduling(Result->ChunkCoordinate, **Chunk);
		return true;
	};

	for (int32 PublishIndex = 0;
		PublishIndex < Data->Config.AsyncSnapshotMaxPublishesPerTick;
		++PublishIndex)
	{
		if (TryPublishChunk())
		{
			continue;
		}
		break;
	}

	while (Stats.CapturedChunks < Data->Config.AsyncSnapshotMaxCapturesPerTick
		&& Data->AsyncState->InFlightCount.Load()
			< Data->Config.AsyncSnapshotMaxConcurrentBuilds
		&& Stats.CheckedScheduleCandidates
			< Data->Config.AsyncSnapshotMaxScheduleCandidatesPerTick)
	{
		FIntVector SelectedCoordinate = FIntVector::ZeroValue;
		bool bActiveCandidate = false;
		if (!Data->PopScheduleNode(
			CurrentTimeSeconds,
			SelectedCoordinate,
			bActiveCandidate))
		{
			break;
		}
		++Stats.CheckedScheduleCandidates;
		if (!bActiveCandidate)
		{
			continue;
		}

		TUniquePtr<FBuildSpatialChunk>* SelectedChunkOwner =
			Data->Chunks.Find(SelectedCoordinate);
		FBuildSpatialChunkAsyncState* SelectedAsyncState =
			Data->ChunkAsyncStates.Find(SelectedCoordinate);
		if (!SelectedChunkOwner || !*SelectedChunkOwner || !SelectedAsyncState)
		{
			Data->RetireChunkScheduling(SelectedCoordinate);
			continue;
		}

		FBuildSpatialChunk& SelectedChunk = **SelectedChunkOwner;
		if (!Data->IsChunkSnapshotEligible(
			SelectedChunk,
			*SelectedAsyncState,
			CurrentTimeSeconds))
		{
			Data->RefreshSnapshotScheduling(SelectedCoordinate, SelectedChunk);
			continue;
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(Building_Snapshot_Capture);
		CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, SnapshotCapture);
		TArray<FBuildSpatialEntry> CapturedEntries;
		SelectedChunk.CaptureStaticEntries(CapturedEntries);
		const uint64 CapturedVersion = SelectedChunk.GetStaticVersion();
		SelectedAsyncState->bInFlight = true;

		TSharedRef<FBuildSpatialAsyncState, ESPMode::ThreadSafe> SharedAsyncState =
			Data->AsyncState.ToSharedRef();
		++SharedAsyncState->InFlightCount;
		Async(
			EAsyncExecution::ThreadPool,
			[SharedAsyncState,
				SelectedCoordinate,
				CapturedVersion,
				CapturedEntries = MoveTemp(CapturedEntries)]() mutable
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Building_Snapshot_Build);
				CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, SnapshotBuild);
				TUniquePtr<FBuildSpatialSnapshotResult> Result =
					MakeUnique<FBuildSpatialSnapshotResult>();
				Result->ChunkCoordinate = SelectedCoordinate;
				Result->StaticVersion = CapturedVersion;
				Result->Snapshot.Build(CapturedEntries);
				if (SharedAsyncState->bActive.Load())
				{
					SharedAsyncState->CompletedResults.Enqueue(MoveTemp(Result));
				}
				--SharedAsyncState->InFlightCount;
			});
		++Stats.CapturedChunks;
	}

	return Stats;
}

int32 FBuildSpatialIndex::RebuildDirtyStaticChunks(const int32 MaxChunkCount)
{
	check(IsInGameThread());
	if (MaxChunkCount <= 0)
	{
		return 0;
	}

	int32 RebuiltChunkCount = 0;
	TArray<FIntVector> ChunkCoordinates;
	ChunkCoordinates.Reserve(FMath::Min(
		MaxChunkCount,
		Data->DirtyStaticChunks.Num()));
	for (const FIntVector& ChunkCoordinate : Data->DirtyStaticChunks)
	{
		ChunkCoordinates.Add(ChunkCoordinate);
		if (ChunkCoordinates.Num() >= MaxChunkCount)
		{
			break;
		}
	}

	for (const FIntVector& ChunkCoordinate : ChunkCoordinates)
	{
		TUniquePtr<FBuildSpatialChunk>* Chunk =
			Data->Chunks.Find(ChunkCoordinate);
		if (!Chunk || !*Chunk)
		{
			Data->RetireChunkScheduling(ChunkCoordinate);
			continue;
		}
		if (!(*Chunk)->IsStaticDirty())
		{
			Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
			continue;
		}

		(*Chunk)->RebuildStaticSnapshot();
		Data->RefreshSnapshotScheduling(ChunkCoordinate, **Chunk);
		++RebuiltChunkCount;
	}
	return RebuiltChunkCount;
}

void FBuildSpatialIndex::Reset()
{
	check(IsInGameThread());
	Data->EntitiesBySlot.Reset();
	Data->EntityCount = 0;
	Data->Chunks.Reset();
	Data->ResetAsyncState();
	Data->BumpQueryRevision();
}
