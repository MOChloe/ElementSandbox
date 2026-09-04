#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "Visual/ElementVisualTypes.h"

struct ELEMENTSANDBOXSIMULATION_API FElementVisualJournalConfig final
{
	int32 MaxRetainedBatchesPerShard = 64;

	bool IsValid() const { return MaxRetainedBatchesPerShard > 0; }
};

class FElementVisualJournalData;

/**
 * 测试 Producer 与以后客户端 Visual 生命周期流共用的纯值容器。每 Shard 独立 Sequence；
 * 事务提交后同时更新 immutable Snapshot+Cursor，再一次性广播变化 Shard。
 */
class ELEMENTSANDBOXSIMULATION_API FElementVisualJournal final : public IElementVisualSource
{
public:
	explicit FElementVisualJournal(
		const FElementVisualJournalConfig& InConfig = FElementVisualJournalConfig());
	virtual ~FElementVisualJournal() override;

	FElementVisualJournal(const FElementVisualJournal&) = delete;
	FElementVisualJournal& operator=(const FElementVisualJournal&) = delete;

	bool BeginTransaction();
	bool CommitTransaction();
	void CancelTransaction();
	bool IsInTransaction() const;

	EElementVisualMutationResult Upsert(const FElementVisualDescriptor& Descriptor);
	EElementVisualMutationResult Remove(
		FElementVisualShardKey Shard,
		FElementVisualKey Key,
		EElementVisualChangeKind Kind,
		uint64 Revision);

	uint64 GetCurrentSequence(FElementVisualShardKey Shard) const;
	const FElementVisualJournalConfig& GetConfig() const;
	SIZE_T GetAllocatedSize() const;

	virtual bool CopyShardSnapshot(
		FElementVisualShardKey Shard,
		FElementVisualShardSnapshot& OutSnapshot) const override;
	virtual EElementVisualJournalReadResult ReadChangesAfter(
		FElementVisualShardKey Shard,
		uint64 Cursor,
		TArray<FElementVisualChangeBatch>& OutBatches) const override;
	virtual void GetKnownShards(TArray<FElementVisualShardKey>& OutShards) const override;
	virtual FElementVisualChangesAvailableEvent& OnChangesAvailable() override
	{
		return ChangesAvailableEvent;
	}

private:
	EElementVisualMutationResult StageUpsert(const FElementVisualDescriptor& Descriptor);
	EElementVisualMutationResult StageRemove(
		FElementVisualShardKey Shard,
		FElementVisualKey Key,
		EElementVisualChangeKind Kind,
		uint64 Revision);
	bool CommitPending();

	TUniquePtr<FElementVisualJournalData> Data;
	FElementVisualChangesAvailableEvent ChangesAvailableEvent;
};
