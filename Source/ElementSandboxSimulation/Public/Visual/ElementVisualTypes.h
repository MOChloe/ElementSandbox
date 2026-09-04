#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Visual/ElementVisualShardKey.h"

/** Persistent Visual 使用 WorldEntityId；Runtime Visual 使用不跨进程的 LeaseId+Generation。 */
struct ELEMENTSANDBOXSIMULATION_API FElementVisualKey final
{
public:
	FElementVisualKey() = default;

	static FElementVisualKey MakePersistent(
		FWorldEntityId WorldEntityId,
		FName VisualKind,
		int32 VisualPartId);
	static FElementVisualKey MakeRuntime(
		uint64 RuntimeLeaseId,
		uint32 RuntimeGeneration,
		FName VisualKind,
		int32 VisualPartId);

	bool IsSet() const;
	bool IsPersistent() const { return WorldEntityId.IsSet(); }
	FWorldEntityId GetWorldEntityId() const { return WorldEntityId; }
	uint64 GetRuntimeLeaseId() const { return RuntimeLeaseId; }
	uint32 GetRuntimeGeneration() const { return RuntimeGeneration; }
	FName GetVisualKind() const { return VisualKind; }
	int32 GetVisualPartId() const { return VisualPartId; }

	friend bool operator==(const FElementVisualKey& Left, const FElementVisualKey& Right)
	{
		return Left.WorldEntityId == Right.WorldEntityId
			&& Left.RuntimeLeaseId == Right.RuntimeLeaseId
			&& Left.RuntimeGeneration == Right.RuntimeGeneration
			&& Left.VisualKind == Right.VisualKind
			&& Left.VisualPartId == Right.VisualPartId;
	}
	friend bool operator!=(const FElementVisualKey& Left, const FElementVisualKey& Right)
	{
		return !(Left == Right);
	}
	friend ELEMENTSANDBOXSIMULATION_API bool operator<(
		const FElementVisualKey& Left,
		const FElementVisualKey& Right);
	friend uint32 GetTypeHash(const FElementVisualKey& Key)
	{
		return HashCombineFast(
			HashCombineFast(GetTypeHash(Key.WorldEntityId), GetTypeHash(Key.RuntimeLeaseId)),
			HashCombineFast(
				HashCombineFast(GetTypeHash(Key.RuntimeGeneration), GetTypeHash(Key.VisualKind)),
				GetTypeHash(Key.VisualPartId)));
	}

private:
	FWorldEntityId WorldEntityId;
	uint64 RuntimeLeaseId = 0;
	uint32 RuntimeGeneration = 0;
	FName VisualKind = NAME_None;
	int32 VisualPartId = INDEX_NONE;
};

/** Simulation/网络 Producer 发布给客户端表现的唯一纯值记录。 */
struct ELEMENTSANDBOXSIMULATION_API FElementVisualDescriptor final
{
	FElementVisualKey Key;
	FName VisualDefinitionId = NAME_None;
	FElementVisualShardKey Shard;
	FTransform WorldTransform = FTransform::Identity;
	FBox WorldBounds = FBox(ForceInit);
	FLinearColor Color = FLinearColor::White;
	float Intensity = 1.0f;
	int64 StartTimeMilliseconds = 0;
	/** 0 表示没有已知结束时间；否则必须严格晚于 StartTime。 */
	int64 EndTimeMilliseconds = 0;
	uint64 Revision = 0;

	bool IsValid() const;
	bool IsEquivalent(const FElementVisualDescriptor& Other) const;
};

enum class EElementVisualChangeKind : uint8
{
	Upsert,
	StateEnded,
	LeaveInterest,
	RuntimeEvict,
	GameplayDestroy
};

inline bool IsElementVisualRemoval(const EElementVisualChangeKind Kind)
{
	return Kind == EElementVisualChangeKind::StateEnded
		|| Kind == EElementVisualChangeKind::LeaveInterest
		|| Kind == EElementVisualChangeKind::RuntimeEvict
		|| Kind == EElementVisualChangeKind::GameplayDestroy;
}

struct ELEMENTSANDBOXSIMULATION_API FElementVisualChange final
{
	EElementVisualChangeKind Kind = EElementVisualChangeKind::Upsert;
	FElementVisualKey Key;
	FElementVisualDescriptor Descriptor;
	uint64 Revision = 0;
};

struct ELEMENTSANDBOXSIMULATION_API FElementVisualChangeBatch final
{
	FElementVisualShardKey Shard;
	uint64 Sequence = 0;
	TArray<FElementVisualChange> Changes;
};

using FElementVisualDescriptorArray = TArray<FElementVisualDescriptor>;

/** Descriptors 指向发布后不再修改的线程安全共享数组，可直接固化进 Worker Request。 */
struct ELEMENTSANDBOXSIMULATION_API FElementVisualShardSnapshot final
{
	FElementVisualShardKey Shard;
	uint64 Cursor = 0;
	TSharedPtr<const FElementVisualDescriptorArray, ESPMode::ThreadSafe> Descriptors;

	const FElementVisualDescriptorArray& GetDescriptors() const;
};

enum class EElementVisualJournalReadResult : uint8
{
	UpToDate,
	Changes,
	Gap
};

enum class EElementVisualMutationResult : uint8
{
	Rejected,
	Unchanged,
	Applied
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FElementVisualChangesAvailableEvent,
	const TArray<FElementVisualShardKey>&);

/** ElementPresentation 只依赖此接口读取事实，不拥有或修改 Producer 的 Gameplay 状态。 */
class ELEMENTSANDBOXSIMULATION_API IElementVisualSource
{
public:
	virtual ~IElementVisualSource() = default;

	virtual bool CopyShardSnapshot(
		FElementVisualShardKey Shard,
		FElementVisualShardSnapshot& OutSnapshot) const = 0;
	virtual EElementVisualJournalReadResult ReadChangesAfter(
		FElementVisualShardKey Shard,
		uint64 Cursor,
		TArray<FElementVisualChangeBatch>& OutBatches) const = 0;
	virtual void GetKnownShards(TArray<FElementVisualShardKey>& OutShards) const = 0;
	virtual FElementVisualChangesAvailableEvent& OnChangesAvailable() = 0;
};
