#pragma once

#include "CoreMinimal.h"
#include "Shape/BuildShapeTypes.h"
#include "Templates/UniquePtr.h"

/** Building 对外发布的中性空间变化；删除语义保持互斥，消费者不得自行猜测。 */
enum class EBuildQuerySnapshotChangeKind : uint8
{
	Upsert,
	Motion,
	Metadata,
	ShapeRemove,
	RuntimeEvict,
	GameplayDestroy,
	LeaveInterest,
	FailedRegistrationRollback
};

/** 一条事务化空间变化同时携带前后快照，供外部执行连续扫掠或原子替换。 */
struct ELEMENTSANDBOXBUILDING_API FBuildQuerySnapshotChange final
{
	EBuildQuerySnapshotChangeKind Kind = EBuildQuerySnapshotChangeKind::Upsert;
	FWorldEntityId WorldEntityId;
	FBuildEntityHandle Entity;
	int32 PartId = INDEX_NONE;
	uint32 StateRevision = 0;
	int64 EffectiveTimeMilliseconds = 0;
	TOptional<FBuildShapeInstanceSnapshot> Previous;
	TOptional<FBuildShapeInstanceSnapshot> Current;

	bool IsValid() const;
};

struct ELEMENTSANDBOXBUILDING_API FBuildQuerySnapshotBatch final
{
	uint64 Sequence = 0;
	TArray<FBuildQuerySnapshotChange> Changes;
};

/**
 * 提交批次在所有同步观察者返回后仍可被有预算的消费者持有；底层数组只构造一次，禁止消费者复制百万级变化。
 */
using FBuildQuerySnapshotBatchRef = TSharedRef<const FBuildQuerySnapshotBatch, ESPMode::ThreadSafe>;

struct ELEMENTSANDBOXBUILDING_API FBuildQuerySnapshotPage final
{
	uint64 Cursor = 0;
	int32 NextOffset = 0;
	bool bHasMore = false;
	TArray<FBuildShapeInstanceSnapshot> Shapes;
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FBuildQuerySnapshotBatchCommittedEvent,
	FBuildQuerySnapshotBatchRef);

class FBuildQuerySnapshotStreamData;

/**
 * Building 的中性、无分片空间快照流。它不理解 Element；事务提交时一次发布不可变前后快照，
 * 消费者可以同步读取，也可以持有同一 SharedRef 在自己的时间预算内推进，不能复制整批 POD。
 */
class ELEMENTSANDBOXBUILDING_API FBuildQuerySnapshotStream final
{
public:
	FBuildQuerySnapshotStream();
	~FBuildQuerySnapshotStream();

	FBuildQuerySnapshotStream(const FBuildQuerySnapshotStream&) = delete;
	FBuildQuerySnapshotStream& operator=(const FBuildQuerySnapshotStream&) = delete;

	bool BeginTransaction();
	bool CommitTransaction();
	void CancelTransaction();
	bool IsInTransaction() const;
	bool Publish(TConstArrayView<FBuildQuerySnapshotChange> Changes);

	bool CopyPage(int32 Offset, int32 MaximumShapes, FBuildQuerySnapshotPage& OutPage) const;
	uint64 GetCurrentSequence() const;
	SIZE_T GetAllocatedSize() const;

	FBuildQuerySnapshotBatchCommittedEvent& OnBatchCommitted() { return BatchCommittedEvent; }

private:
	bool CommitPending();

	TUniquePtr<FBuildQuerySnapshotStreamData> Data;
	FBuildQuerySnapshotBatchCommittedEvent BatchCommittedEvent;
};
