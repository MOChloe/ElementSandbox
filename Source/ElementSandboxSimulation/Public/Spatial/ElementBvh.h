#pragma once

#include "CoreMinimal.h"
#include "Shape/ElementCompoundShape.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

struct ELEMENTSANDBOXSIMULATION_API FElementBvhStats final
{
	int32 LeafCount = 0;
	int32 NodeCount = 0;
	uint64 QueryCount = 0;
	uint64 NodeVisitCount = 0;
	uint64 CandidateCount = 0;
	uint64 RefitCount = 0;
	uint64 RebuildCount = 0;
	uint64 BackgroundRebuildScheduledCount = 0;
	uint64 BackgroundRebuildPublishedCount = 0;
	uint64 BackgroundRebuildDiscardedCount = 0;
	double QualityRatio = 1.0;
};

class FElementBvhSnapshotData;

/** Worker 只持有不可变 Snapshot；查询返回 Snapshot Handle，不返回可失效的行指针。 */
class ELEMENTSANDBOXSIMULATION_API FElementBvhSnapshot final
{
public:
	FElementBvhSnapshot();
	~FElementBvhSnapshot();
	FElementBvhSnapshot(FElementBvhSnapshot&&) noexcept;
	FElementBvhSnapshot& operator=(FElementBvhSnapshot&&) noexcept;

	FElementBvhSnapshot(const FElementBvhSnapshot&) = delete;
	FElementBvhSnapshot& operator=(const FElementBvhSnapshot&) = delete;

	void Query(const FBox& Bounds, TArray<FElementSpatialSnapshotHandle>& OutHandles, FElementBvhStats* Stats = nullptr) const;
	int32 Num() const;

private:
	TUniquePtr<FElementBvhSnapshotData> Data;
	friend class FElementBvh;
};

class FElementBvhData;

enum class EElementBvhPublishMode : uint8
{
	/** Influence 等权威输入在调用返回前必须进入可查询 Snapshot。 */
	Immediate,
	/** Target 由 Pending Refresh 精确兜底，允许大型拓扑在 Worker 构建后原子发布。 */
	DeferredLargeTopology
};

/** Dynamic AABB BVH：既有叶只做批量 refit；拓扑变化或质量下降时重建并原子发布新 Snapshot。 */
class ELEMENTSANDBOXSIMULATION_API FElementBvh final
{
public:
	FElementBvh();
	~FElementBvh();

	FElementBvh(const FElementBvh&) = delete;
	FElementBvh& operator=(const FElementBvh&) = delete;

	/** 大批初始化可预留叶容量；不创建叶也不改变已发布 Snapshot。 */
	void Reserve(int32 LeafCapacity);
	FElementSpatialSnapshotHandle Insert(const FBox& Bounds);
	bool Update(FElementSpatialSnapshotHandle Handle, const FBox& Bounds);
	bool Remove(FElementSpatialSnapshotHandle Handle);
	bool IsAlive(FElementSpatialSnapshotHandle Handle) const;
	bool TryGetBounds(FElementSpatialSnapshotHandle Handle, FBox& OutBounds) const;

	/**
	 * 没有变化时是零分配空操作；发布后旧 Snapshot 可继续被正在运行的 Worker 使用。
	 * Deferred 只允许用于另有精确增量兜底的 Target 索引，Influence 不得延迟。
	 */
	bool PublishSnapshot(EElementBvhPublishMode Mode = EElementBvhPublishMode::Immediate);
	TSharedPtr<const FElementBvhSnapshot, ESPMode::ThreadSafe> GetPublishedSnapshot() const;
	const FElementBvhStats& GetStats() const;
	SIZE_T GetAllocatedSize() const;

private:
	TUniquePtr<FElementBvhData> Data;
};
