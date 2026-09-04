#include "Snapshot/BuildQuerySnapshotStream.h"

namespace
{
	/**
	 * 百万级快照不能放在单个 TMap 中：一次桶扩容会在 Game Thread 重哈希整张表。
	 * WorldEntityId 分片把最坏扩容限制在总集合的 1/256，同时保持晚加入消费者所需的完整当前态。
	 */
	class FBuildQueryShapeStore final
	{
	public:
		void Add(const FBuildShapeRef& Ref, const FBuildShapeInstanceSnapshot& Snapshot)
		{
			TMap<FBuildShapeRef, FBuildShapeInstanceSnapshot>& Shard = Shards[GetShardIndex(Ref)];
			ShapeCount += Shard.Contains(Ref) ? 0 : 1;
			Shard.Add(Ref, Snapshot);
		}

		void Remove(const FBuildShapeRef& Ref)
		{
			ShapeCount -= Shards[GetShardIndex(Ref)].Remove(Ref);
		}

		const FBuildShapeInstanceSnapshot& FindChecked(const FBuildShapeRef& Ref) const
		{
			return Shards[GetShardIndex(Ref)].FindChecked(Ref);
		}

		void GenerateKeyArray(TArray<FBuildShapeRef>& OutKeys) const
		{
			OutKeys.Reset(ShapeCount);
			for (const TMap<FBuildShapeRef, FBuildShapeInstanceSnapshot>& Shard : Shards)
			{
				for (const TPair<FBuildShapeRef, FBuildShapeInstanceSnapshot>& Pair : Shard)
				{
					OutKeys.Add(Pair.Key);
				}
			}
		}

		SIZE_T GetAllocatedSize() const
		{
			SIZE_T Bytes = 0;
			for (const TMap<FBuildShapeRef, FBuildShapeInstanceSnapshot>& Shard : Shards)
			{
				Bytes += Shard.GetAllocatedSize();
			}
			return Bytes;
		}

	private:
		static constexpr uint32 ShardCount = 256;
		static uint32 GetShardIndex(const FBuildShapeRef& Ref)
		{
			return GetTypeHash(Ref.WorldEntityId) & (ShardCount - 1);
		}

		TStaticArray<TMap<FBuildShapeRef, FBuildShapeInstanceSnapshot>, ShardCount> Shards;
		int32 ShapeCount = 0;
	};
}

class FBuildQuerySnapshotStreamData final
{
public:
	uint64 Sequence = 0;
	FBuildQueryShapeStore CurrentShapes;
	TArray<FBuildQuerySnapshotChange> Pending;
	bool bInTransaction = false;
};

namespace
{
	uint64 AdvanceNonZero(const uint64 Value)
	{
		return Value == MAX_uint64 ? 1 : Value + 1;
	}

	bool ShapeRefLess(const FBuildShapeRef& Left, const FBuildShapeRef& Right)
	{
		if (Left.WorldEntityId != Right.WorldEntityId) return Left.WorldEntityId < Right.WorldEntityId;
		if (Left.Entity.GetRegistryId() != Right.Entity.GetRegistryId())
			return Left.Entity.GetRegistryId() < Right.Entity.GetRegistryId();
		if (Left.Entity.GetIndex() != Right.Entity.GetIndex()) return Left.Entity.GetIndex() < Right.Entity.GetIndex();
		if (Left.Entity.GetGeneration() != Right.Entity.GetGeneration())
			return Left.Entity.GetGeneration() < Right.Entity.GetGeneration();
		if (Left.PartId != Right.PartId) return Left.PartId < Right.PartId;
		return Left.ShapeId < Right.ShapeId;
	}
}

bool FBuildQuerySnapshotChange::IsValid() const
{
	const FBuildShapeInstanceSnapshot* Identity = Current.IsSet() ? &Current.GetValue()
		: (Previous.IsSet() ? &Previous.GetValue() : nullptr);
	if (!Identity || !Identity->IsValid() || !WorldEntityId.IsSet() || !Entity.IsSet()
		|| PartId < 0 || StateRevision == 0 || EffectiveTimeMilliseconds < 0) return false;
	if (Identity->ShapeRef.WorldEntityId != WorldEntityId || Identity->ShapeRef.Entity != Entity
		|| Identity->ShapeRef.PartId != PartId) return false;
	if (Previous.IsSet() && !Previous->IsValid()) return false;
	if (Current.IsSet() && !Current->IsValid()) return false;
	const bool bRemoval = Kind == EBuildQuerySnapshotChangeKind::ShapeRemove
		|| Kind == EBuildQuerySnapshotChangeKind::RuntimeEvict
		|| Kind == EBuildQuerySnapshotChangeKind::GameplayDestroy
		|| Kind == EBuildQuerySnapshotChangeKind::LeaveInterest
		|| Kind == EBuildQuerySnapshotChangeKind::FailedRegistrationRollback;
	return bRemoval ? Previous.IsSet() && !Current.IsSet() : Current.IsSet();
}

FBuildQuerySnapshotStream::FBuildQuerySnapshotStream()
	: Data(MakeUnique<FBuildQuerySnapshotStreamData>())
{
}

FBuildQuerySnapshotStream::~FBuildQuerySnapshotStream() = default;

bool FBuildQuerySnapshotStream::BeginTransaction()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction) return false;
	check(Data->Pending.IsEmpty());
	Data->bInTransaction = true;
	return true;
}

bool FBuildQuerySnapshotStream::CommitTransaction()
{
	check(IsInGameThread());
	if (!Data || !Data->bInTransaction) return false;
	Data->bInTransaction = false;
	return CommitPending();
}

void FBuildQuerySnapshotStream::CancelTransaction()
{
	check(IsInGameThread());
	if (!Data) return;
	Data->Pending.Reset();
	Data->bInTransaction = false;
}

bool FBuildQuerySnapshotStream::IsInTransaction() const
{
	return Data && Data->bInTransaction;
}

bool FBuildQuerySnapshotStream::Publish(const TConstArrayView<FBuildQuerySnapshotChange> Changes)
{
	check(IsInGameThread());
	if (!Data || Changes.IsEmpty()) return false;
	for (const FBuildQuerySnapshotChange& Change : Changes)
	{
		if (!Change.IsValid()) return false;
	}
	Data->Pending.Append(Changes.GetData(), Changes.Num());
	return Data->bInTransaction || CommitPending();
}

bool FBuildQuerySnapshotStream::CommitPending()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction) return false;
	if (Data->Pending.IsEmpty()) return true;
	TSharedRef<FBuildQuerySnapshotBatch, ESPMode::ThreadSafe> MutableBatch =
		MakeShared<FBuildQuerySnapshotBatch, ESPMode::ThreadSafe>();
	MutableBatch->Sequence = AdvanceNonZero(Data->Sequence);
	MutableBatch->Changes = MoveTemp(Data->Pending);
	Data->Pending.Reset();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_QuerySnapshots_ApplyCurrentShapes);
		for (const FBuildQuerySnapshotChange& Change : MutableBatch->Changes)
		{
			const FBuildShapeRef& Ref = Change.Current.IsSet()
				? Change.Current->ShapeRef : Change.Previous->ShapeRef;
			if (Change.Current.IsSet()) Data->CurrentShapes.Add(Ref, Change.Current.GetValue());
			else Data->CurrentShapes.Remove(Ref);
		}
	}
	Data->Sequence = MutableBatch->Sequence;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Building_QuerySnapshots_Broadcast);
		const FBuildQuerySnapshotBatchRef ImmutableBatch = MutableBatch;
		BatchCommittedEvent.Broadcast(ImmutableBatch);
	}
	return true;
}

bool FBuildQuerySnapshotStream::CopyPage(
	const int32 Offset,
	const int32 MaximumShapes,
	FBuildQuerySnapshotPage& OutPage) const
{
	check(IsInGameThread());
	OutPage = {};
	if (!Data || Offset < 0 || MaximumShapes <= 0) return false;
	TArray<FBuildShapeRef> Keys;
	Data->CurrentShapes.GenerateKeyArray(Keys);
	Keys.Sort(ShapeRefLess);
	if (Offset > Keys.Num()) return false;
	OutPage.Cursor = Data->Sequence;
	const int32 End = FMath::Min(Offset + MaximumShapes, Keys.Num());
	OutPage.Shapes.Reserve(End - Offset);
	for (int32 Index = Offset; Index < End; ++Index)
	{
		OutPage.Shapes.Add(Data->CurrentShapes.FindChecked(Keys[Index]));
	}
	OutPage.NextOffset = End;
	OutPage.bHasMore = End < Keys.Num();
	return true;
}

uint64 FBuildQuerySnapshotStream::GetCurrentSequence() const
{
	return Data ? Data->Sequence : 0;
}

SIZE_T FBuildQuerySnapshotStream::GetAllocatedSize() const
{
	if (!Data) return 0;
	return Data->CurrentShapes.GetAllocatedSize() + Data->Pending.GetAllocatedSize();
}
