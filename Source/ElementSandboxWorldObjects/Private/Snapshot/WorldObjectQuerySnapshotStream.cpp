#include "Snapshot/WorldObjectQuerySnapshotStream.h"

namespace
{
	/** 当前 Shape 集按 WorldEntityId 分片，阻止一次 TMap 扩容重哈希全部驻留 WorldObject。 */
	class FWorldObjectQueryShapeStore final
	{
	public:
		void Add(const FWorldObjectShapeRef& Ref, const FWorldObjectShapeInstanceSnapshot& Snapshot)
		{
			TMap<FWorldObjectShapeRef, FWorldObjectShapeInstanceSnapshot>& Shard = Shards[GetShardIndex(Ref)];
			ShapeCount += Shard.Contains(Ref) ? 0 : 1;
			Shard.Add(Ref, Snapshot);
		}

		void Remove(const FWorldObjectShapeRef& Ref)
		{
			ShapeCount -= Shards[GetShardIndex(Ref)].Remove(Ref);
		}

		const FWorldObjectShapeInstanceSnapshot& FindChecked(const FWorldObjectShapeRef& Ref) const
		{
			return Shards[GetShardIndex(Ref)].FindChecked(Ref);
		}

		void GenerateKeyArray(TArray<FWorldObjectShapeRef>& OutKeys) const
		{
			OutKeys.Reset(ShapeCount);
			for (const TMap<FWorldObjectShapeRef, FWorldObjectShapeInstanceSnapshot>& Shard : Shards)
			{
				for (const TPair<FWorldObjectShapeRef, FWorldObjectShapeInstanceSnapshot>& Pair : Shard)
				{
					OutKeys.Add(Pair.Key);
				}
			}
		}

		SIZE_T GetAllocatedSize() const
		{
			SIZE_T Bytes = 0;
			for (const TMap<FWorldObjectShapeRef, FWorldObjectShapeInstanceSnapshot>& Shard : Shards)
			{
				Bytes += Shard.GetAllocatedSize();
			}
			return Bytes;
		}

	private:
		static constexpr uint32 ShardCount = 256;
		static uint32 GetShardIndex(const FWorldObjectShapeRef& Ref)
		{
			return GetTypeHash(Ref.WorldEntityId) & (ShardCount - 1);
		}

		TStaticArray<TMap<FWorldObjectShapeRef, FWorldObjectShapeInstanceSnapshot>, ShardCount> Shards;
		int32 ShapeCount = 0;
	};
}

class FWorldObjectQuerySnapshotStreamData final
{
public:
	uint64 Sequence = 0;
	FWorldObjectQueryShapeStore CurrentShapes;
	TArray<FWorldObjectQuerySnapshotChange> Pending;
	bool bInTransaction = false;
};

namespace
{
	uint64 AdvanceNonZero(const uint64 Value) { return Value == MAX_uint64 ? 1 : Value + 1; }

	bool ShapeRefLess(const FWorldObjectShapeRef& Left, const FWorldObjectShapeRef& Right)
	{
		if (Left.WorldEntityId != Right.WorldEntityId) return Left.WorldEntityId < Right.WorldEntityId;
		if (Left.Entity.GetRegistryId() != Right.Entity.GetRegistryId())
			return Left.Entity.GetRegistryId() < Right.Entity.GetRegistryId();
		if (Left.Entity.GetSlot() != Right.Entity.GetSlot()) return Left.Entity.GetSlot() < Right.Entity.GetSlot();
		if (Left.Entity.GetGeneration() != Right.Entity.GetGeneration())
			return Left.Entity.GetGeneration() < Right.Entity.GetGeneration();
		return Left.ShapeId < Right.ShapeId;
	}
}

bool FWorldObjectQuerySnapshotChange::IsValid() const
{
	const FWorldObjectShapeInstanceSnapshot* Identity = Current.IsSet() ? &Current.GetValue()
		: (Previous.IsSet() ? &Previous.GetValue() : nullptr);
	if (!Identity || !Identity->IsValid() || !WorldEntityId.IsSet() || !Entity.IsSet()
		|| StateRevision == 0 || EffectiveTimeMilliseconds < 0) return false;
	if (Identity->ShapeRef.WorldEntityId != WorldEntityId || Identity->ShapeRef.Entity != Entity) return false;
	if (Previous.IsSet() && !Previous->IsValid()) return false;
	if (Current.IsSet() && !Current->IsValid()) return false;
	const bool bRemoval = Kind == EWorldObjectQuerySnapshotChangeKind::ShapeRemove
		|| Kind == EWorldObjectQuerySnapshotChangeKind::RuntimeEvict
		|| Kind == EWorldObjectQuerySnapshotChangeKind::GameplayDestroy
		|| Kind == EWorldObjectQuerySnapshotChangeKind::LeaveInterest
		|| Kind == EWorldObjectQuerySnapshotChangeKind::FailedRegistrationRollback;
	return bRemoval ? Previous.IsSet() && !Current.IsSet() : Current.IsSet();
}

FWorldObjectQuerySnapshotStream::FWorldObjectQuerySnapshotStream()
	: Data(MakeUnique<FWorldObjectQuerySnapshotStreamData>())
{
}

FWorldObjectQuerySnapshotStream::~FWorldObjectQuerySnapshotStream() = default;

bool FWorldObjectQuerySnapshotStream::BeginTransaction()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction) return false;
	check(Data->Pending.IsEmpty());
	Data->bInTransaction = true;
	return true;
}

bool FWorldObjectQuerySnapshotStream::CommitTransaction()
{
	check(IsInGameThread());
	if (!Data || !Data->bInTransaction) return false;
	Data->bInTransaction = false;
	return CommitPending();
}

void FWorldObjectQuerySnapshotStream::CancelTransaction()
{
	check(IsInGameThread());
	if (!Data) return;
	Data->Pending.Reset();
	Data->bInTransaction = false;
}

bool FWorldObjectQuerySnapshotStream::IsInTransaction() const
{
	return Data && Data->bInTransaction;
}

bool FWorldObjectQuerySnapshotStream::Publish(const TConstArrayView<FWorldObjectQuerySnapshotChange> Changes)
{
	check(IsInGameThread());
	if (!Data || Changes.IsEmpty()) return false;
	for (const FWorldObjectQuerySnapshotChange& Change : Changes)
	{
		if (!Change.IsValid()) return false;
	}
	Data->Pending.Append(Changes.GetData(), Changes.Num());
	return Data->bInTransaction || CommitPending();
}

bool FWorldObjectQuerySnapshotStream::CommitPending()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction) return false;
	if (Data->Pending.IsEmpty()) return true;
	FWorldObjectQuerySnapshotBatch Batch;
	Batch.Sequence = AdvanceNonZero(Data->Sequence);
	Batch.Changes = MoveTemp(Data->Pending);
	Data->Pending.Reset();
	for (const FWorldObjectQuerySnapshotChange& Change : Batch.Changes)
	{
		const FWorldObjectShapeRef& Ref = Change.Current.IsSet()
			? Change.Current->ShapeRef : Change.Previous->ShapeRef;
		if (Change.Current.IsSet()) Data->CurrentShapes.Add(Ref, Change.Current.GetValue());
		else Data->CurrentShapes.Remove(Ref);
	}
	Data->Sequence = Batch.Sequence;
	BatchCommittedEvent.Broadcast(Batch);
	return true;
}

bool FWorldObjectQuerySnapshotStream::CopyPage(
	const int32 Offset,
	const int32 MaximumShapes,
	FWorldObjectQuerySnapshotPage& OutPage) const
{
	check(IsInGameThread());
	OutPage = {};
	if (!Data || Offset < 0 || MaximumShapes <= 0) return false;
	TArray<FWorldObjectShapeRef> Keys;
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

uint64 FWorldObjectQuerySnapshotStream::GetCurrentSequence() const
{
	return Data ? Data->Sequence : 0;
}

SIZE_T FWorldObjectQuerySnapshotStream::GetAllocatedSize() const
{
	return Data ? Data->CurrentShapes.GetAllocatedSize() + Data->Pending.GetAllocatedSize() : 0;
}
