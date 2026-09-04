#include "Visual/ElementVisualJournal.h"

namespace
{
	struct FElementVisualShardJournal final
	{
		uint64 CurrentSequence = 0;
		TMap<FElementVisualKey, FElementVisualDescriptor> Current;
		TSharedPtr<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Snapshot;
		TArray<FElementVisualChangeBatch> Batches;
	};

	uint64 NextSequence(const uint64 Current)
	{
		return Current == MAX_uint64 ? 1 : Current + 1;
	}

	void BuildImmutableSnapshot(FElementVisualShardJournal& Journal)
	{
		TSharedRef<FElementVisualDescriptorArray, ESPMode::ThreadSafe> Values =
			MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
		Values->Reserve(Journal.Current.Num());
		for (const TPair<FElementVisualKey, FElementVisualDescriptor>& Pair : Journal.Current)
		{
			Values->Add(Pair.Value);
		}
		Values->Sort([](const FElementVisualDescriptor& Left, const FElementVisualDescriptor& Right)
		{
			return Left.Key < Right.Key;
		});
		Journal.Snapshot = Values;
	}
}

class FElementVisualJournalData final
{
public:
	explicit FElementVisualJournalData(const FElementVisualJournalConfig& InConfig)
		: Config(InConfig)
	{
		check(Config.IsValid());
	}

	const FElementVisualDescriptor* ResolveDescriptor(const FElementVisualKey& Key) const
	{
		if (const TOptional<FElementVisualShardKey>* PendingLocation = PendingLocationByKey.Find(Key))
		{
			if (!PendingLocation->IsSet())
			{
				return nullptr;
			}
			if (const TArray<FElementVisualChange>* Changes = PendingByShard.Find(PendingLocation->GetValue()))
			{
				for (int32 Index = Changes->Num() - 1; Index >= 0; --Index)
				{
					if ((*Changes)[Index].Key == Key)
					{
						return (*Changes)[Index].Kind == EElementVisualChangeKind::Upsert
							? &(*Changes)[Index].Descriptor
							: nullptr;
					}
				}
			}
		}
		const FElementVisualShardKey* Shard = LocationByKey.Find(Key);
		const FElementVisualShardJournal* Journal = Shard ? Shards.Find(*Shard) : nullptr;
		return Journal ? Journal->Current.Find(Key) : nullptr;
	}

	TOptional<FElementVisualShardKey> ResolveLocation(const FElementVisualKey& Key) const
	{
		if (const TOptional<FElementVisualShardKey>* Pending = PendingLocationByKey.Find(Key))
		{
			return *Pending;
		}
		if (const FElementVisualShardKey* Existing = LocationByKey.Find(Key))
		{
			return *Existing;
		}
		return {};
	}

	FElementVisualJournalConfig Config;
	TMap<FElementVisualShardKey, FElementVisualShardJournal> Shards;
	TMap<FElementVisualKey, FElementVisualShardKey> LocationByKey;
	TMap<FElementVisualShardKey, TArray<FElementVisualChange>> PendingByShard;
	TMap<FElementVisualKey, TOptional<FElementVisualShardKey>> PendingLocationByKey;
	bool bInTransaction = false;
};

FElementVisualJournal::FElementVisualJournal(const FElementVisualJournalConfig& InConfig)
	: Data(MakeUnique<FElementVisualJournalData>(InConfig))
{
}

FElementVisualJournal::~FElementVisualJournal() = default;

bool FElementVisualJournal::BeginTransaction()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction)
	{
		return false;
	}
	check(Data->PendingByShard.IsEmpty() && Data->PendingLocationByKey.IsEmpty());
	Data->bInTransaction = true;
	return true;
}

bool FElementVisualJournal::CommitTransaction()
{
	check(IsInGameThread());
	if (!Data || !Data->bInTransaction)
	{
		return false;
	}
	Data->bInTransaction = false;
	return CommitPending();
}

void FElementVisualJournal::CancelTransaction()
{
	check(IsInGameThread());
	if (Data)
	{
		Data->PendingByShard.Reset();
		Data->PendingLocationByKey.Reset();
		Data->bInTransaction = false;
	}
}

bool FElementVisualJournal::IsInTransaction() const
{
	return Data && Data->bInTransaction;
}

EElementVisualMutationResult FElementVisualJournal::Upsert(
	const FElementVisualDescriptor& Descriptor)
{
	check(IsInGameThread());
	const EElementVisualMutationResult Result = StageUpsert(Descriptor);
	if (Result != EElementVisualMutationResult::Applied || Data->bInTransaction)
	{
		return Result;
	}
	return CommitPending() ? Result : EElementVisualMutationResult::Rejected;
}

EElementVisualMutationResult FElementVisualJournal::StageUpsert(
	const FElementVisualDescriptor& Descriptor)
{
	if (!Data || !Descriptor.IsValid())
	{
		return EElementVisualMutationResult::Rejected;
	}
	const TOptional<FElementVisualShardKey> Location = Data->ResolveLocation(Descriptor.Key);
	if (Location.IsSet() && Location.GetValue() != Descriptor.Shard)
	{
		return EElementVisualMutationResult::Rejected;
	}
	if (const FElementVisualDescriptor* Existing = Data->ResolveDescriptor(Descriptor.Key))
	{
		if (Descriptor.Revision < Existing->Revision)
		{
			return EElementVisualMutationResult::Unchanged;
		}
		if (Descriptor.Revision == Existing->Revision)
		{
			return Descriptor.IsEquivalent(*Existing)
				? EElementVisualMutationResult::Unchanged
				: EElementVisualMutationResult::Rejected;
		}
	}

	FElementVisualChange& Change = Data->PendingByShard.FindOrAdd(Descriptor.Shard).AddDefaulted_GetRef();
	Change.Kind = EElementVisualChangeKind::Upsert;
	Change.Key = Descriptor.Key;
	Change.Descriptor = Descriptor;
	Change.Revision = Descriptor.Revision;
	Data->PendingLocationByKey.Add(Descriptor.Key, Descriptor.Shard);
	return EElementVisualMutationResult::Applied;
}

EElementVisualMutationResult FElementVisualJournal::Remove(
	const FElementVisualShardKey Shard,
	const FElementVisualKey Key,
	const EElementVisualChangeKind Kind,
	const uint64 Revision)
{
	check(IsInGameThread());
	const EElementVisualMutationResult Result = StageRemove(Shard, Key, Kind, Revision);
	if (Result != EElementVisualMutationResult::Applied || Data->bInTransaction)
	{
		return Result;
	}
	return CommitPending() ? Result : EElementVisualMutationResult::Rejected;
}

EElementVisualMutationResult FElementVisualJournal::StageRemove(
	const FElementVisualShardKey Shard,
	const FElementVisualKey Key,
	const EElementVisualChangeKind Kind,
	const uint64 Revision)
{
	if (!Data || !Key.IsSet() || !IsElementVisualRemoval(Kind) || Revision == 0)
	{
		return EElementVisualMutationResult::Rejected;
	}
	const TOptional<FElementVisualShardKey> Location = Data->ResolveLocation(Key);
	if (!Location.IsSet())
	{
		return EElementVisualMutationResult::Unchanged;
	}
	if (Location.GetValue() != Shard)
	{
		return EElementVisualMutationResult::Rejected;
	}
	const FElementVisualDescriptor* Existing = Data->ResolveDescriptor(Key);
	if (!Existing)
	{
		return EElementVisualMutationResult::Unchanged;
	}
	if (Revision <= Existing->Revision)
	{
		return Revision == Existing->Revision
			? EElementVisualMutationResult::Unchanged
			: EElementVisualMutationResult::Rejected;
	}

	FElementVisualChange& Change = Data->PendingByShard.FindOrAdd(Shard).AddDefaulted_GetRef();
	Change.Kind = Kind;
	Change.Key = Key;
	Change.Revision = Revision;
	Data->PendingLocationByKey.Add(Key, TOptional<FElementVisualShardKey>());
	return EElementVisualMutationResult::Applied;
}

bool FElementVisualJournal::CommitPending()
{
	check(IsInGameThread());
	if (!Data || Data->bInTransaction)
	{
		return false;
	}
	if (Data->PendingByShard.IsEmpty())
	{
		Data->PendingLocationByKey.Reset();
		return true;
	}

	TArray<FElementVisualShardKey> ChangedShards;
	Data->PendingByShard.GenerateKeyArray(ChangedShards);
	ChangedShards.Sort();
	for (const FElementVisualShardKey Shard : ChangedShards)
	{
		TArray<FElementVisualChange>* Pending = Data->PendingByShard.Find(Shard);
		if (!Pending || Pending->IsEmpty())
		{
			continue;
		}
		FElementVisualShardJournal& Journal = Data->Shards.FindOrAdd(Shard);
			for (const FElementVisualChange& Change : *Pending)
			{
				if (Change.Kind == EElementVisualChangeKind::Upsert)
				{
					Journal.Current.Add(Change.Key, Change.Descriptor);
				}
				else
				{
					Journal.Current.Remove(Change.Key);
				}
			}
		Journal.CurrentSequence = NextSequence(Journal.CurrentSequence);
		BuildImmutableSnapshot(Journal);
		FElementVisualChangeBatch& Batch = Journal.Batches.AddDefaulted_GetRef();
		Batch.Shard = Shard;
		Batch.Sequence = Journal.CurrentSequence;
		Batch.Changes = MoveTemp(*Pending);
		const int32 Excess = Journal.Batches.Num() - Data->Config.MaxRetainedBatchesPerShard;
		if (Excess > 0)
		{
			Journal.Batches.RemoveAt(0, Excess, EAllowShrinking::No);
		}
		}
		for (const TPair<FElementVisualKey, TOptional<FElementVisualShardKey>>& Pair : Data->PendingLocationByKey)
		{
			if (Pair.Value.IsSet())
			{
				Data->LocationByKey.Add(Pair.Key, Pair.Value.GetValue());
			}
			else
			{
				Data->LocationByKey.Remove(Pair.Key);
			}
		}
		Data->PendingByShard.Reset();
	Data->PendingLocationByKey.Reset();
	ChangesAvailableEvent.Broadcast(ChangedShards);
	return true;
}

uint64 FElementVisualJournal::GetCurrentSequence(const FElementVisualShardKey Shard) const
{
	check(IsInGameThread());
	const FElementVisualShardJournal* Journal = Data ? Data->Shards.Find(Shard) : nullptr;
	return Journal ? Journal->CurrentSequence : 0;
}

bool FElementVisualJournal::CopyShardSnapshot(
	const FElementVisualShardKey Shard,
	FElementVisualShardSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	OutSnapshot.Shard = Shard;
	const FElementVisualShardJournal* Journal = Data ? Data->Shards.Find(Shard) : nullptr;
	if (!Journal)
	{
		OutSnapshot.Descriptors = MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
		return Data != nullptr;
	}
	OutSnapshot.Cursor = Journal->CurrentSequence;
	OutSnapshot.Descriptors = Journal->Snapshot.IsValid()
		? Journal->Snapshot
		: MakeShared<FElementVisualDescriptorArray, ESPMode::ThreadSafe>();
	return true;
}

EElementVisualJournalReadResult FElementVisualJournal::ReadChangesAfter(
	const FElementVisualShardKey Shard,
	const uint64 Cursor,
	TArray<FElementVisualChangeBatch>& OutBatches) const
{
	check(IsInGameThread());
	OutBatches.Reset();
	const FElementVisualShardJournal* Journal = Data ? Data->Shards.Find(Shard) : nullptr;
	if (!Journal)
	{
		return Cursor == 0
			? EElementVisualJournalReadResult::UpToDate
			: EElementVisualJournalReadResult::Gap;
	}
	if (Cursor == Journal->CurrentSequence)
	{
		return EElementVisualJournalReadResult::UpToDate;
	}
	if (Cursor > Journal->CurrentSequence || Journal->Batches.IsEmpty()
		|| Cursor + 1 < Journal->Batches[0].Sequence)
	{
		return EElementVisualJournalReadResult::Gap;
	}
	for (const FElementVisualChangeBatch& Batch : Journal->Batches)
	{
		if (Batch.Sequence > Cursor)
		{
			OutBatches.Add(Batch);
		}
	}
	return OutBatches.IsEmpty()
		? EElementVisualJournalReadResult::UpToDate
		: EElementVisualJournalReadResult::Changes;
}

void FElementVisualJournal::GetKnownShards(TArray<FElementVisualShardKey>& OutShards) const
{
	check(IsInGameThread());
	OutShards.Reset();
	if (Data)
	{
		Data->Shards.GenerateKeyArray(OutShards);
		OutShards.Sort();
	}
}

const FElementVisualJournalConfig& FElementVisualJournal::GetConfig() const
{
	check(Data);
	return Data->Config;
}

SIZE_T FElementVisualJournal::GetAllocatedSize() const
{
	if (!Data)
	{
		return 0;
	}
	SIZE_T Size = Data->Shards.GetAllocatedSize()
		+ Data->LocationByKey.GetAllocatedSize()
		+ Data->PendingByShard.GetAllocatedSize()
		+ Data->PendingLocationByKey.GetAllocatedSize();
	for (const TPair<FElementVisualShardKey, FElementVisualShardJournal>& Pair : Data->Shards)
	{
		Size += Pair.Value.Current.GetAllocatedSize()
			+ Pair.Value.Batches.GetAllocatedSize();
		if (Pair.Value.Snapshot.IsValid())
		{
			Size += Pair.Value.Snapshot->GetAllocatedSize();
		}
		for (const FElementVisualChangeBatch& Batch : Pair.Value.Batches)
		{
			Size += Batch.Changes.GetAllocatedSize();
		}
	}
	return Size;
}
