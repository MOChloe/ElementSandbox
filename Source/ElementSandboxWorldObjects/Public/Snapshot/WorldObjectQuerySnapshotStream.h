#pragma once

#include "CoreMinimal.h"
#include "Shape/WorldObjectShapeTypes.h"
#include "Templates/UniquePtr.h"

enum class EWorldObjectQuerySnapshotChangeKind : uint8
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

/** WorldObject 对外发布的中性前后空间快照，不包含任何 Element/Fire 语义。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectQuerySnapshotChange final
{
	EWorldObjectQuerySnapshotChangeKind Kind = EWorldObjectQuerySnapshotChangeKind::Upsert;
	FWorldEntityId WorldEntityId;
	FWorldObjectEntityHandle Entity;
	uint32 StateRevision = 0;
	int64 EffectiveTimeMilliseconds = 0;
	TOptional<FWorldObjectShapeInstanceSnapshot> Previous;
	TOptional<FWorldObjectShapeInstanceSnapshot> Current;

	bool IsValid() const;
};

struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectQuerySnapshotBatch final
{
	uint64 Sequence = 0;
	TArray<FWorldObjectQuerySnapshotChange> Changes;
};

struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectQuerySnapshotPage final
{
	uint64 Cursor = 0;
	int32 NextOffset = 0;
	bool bHasMore = false;
	TArray<FWorldObjectShapeInstanceSnapshot> Shapes;
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FWorldObjectQuerySnapshotBatchCommittedEvent,
	const FWorldObjectQuerySnapshotBatch&);

class FWorldObjectQuerySnapshotStreamData;

/** 事务化、无 Shard Interest 的 WorldObject 中性空间快照流。 */
class ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectQuerySnapshotStream final
{
public:
	FWorldObjectQuerySnapshotStream();
	~FWorldObjectQuerySnapshotStream();

	FWorldObjectQuerySnapshotStream(const FWorldObjectQuerySnapshotStream&) = delete;
	FWorldObjectQuerySnapshotStream& operator=(const FWorldObjectQuerySnapshotStream&) = delete;

	bool BeginTransaction();
	bool CommitTransaction();
	void CancelTransaction();
	bool IsInTransaction() const;
	bool Publish(TConstArrayView<FWorldObjectQuerySnapshotChange> Changes);

	bool CopyPage(int32 Offset, int32 MaximumShapes, FWorldObjectQuerySnapshotPage& OutPage) const;
	uint64 GetCurrentSequence() const;
	SIZE_T GetAllocatedSize() const;

	FWorldObjectQuerySnapshotBatchCommittedEvent& OnBatchCommitted() { return BatchCommittedEvent; }

private:
	bool CommitPending();
	TUniquePtr<FWorldObjectQuerySnapshotStreamData> Data;
	FWorldObjectQuerySnapshotBatchCommittedEvent BatchCommittedEvent;
};

