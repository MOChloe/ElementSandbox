#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Templates/UniquePtr.h"

#include "BuildSpatialIndex.generated.h"

class FBuildSpatialIndexData;

/** 空间更新策略，与 HISM/Actor 等表现载体无关。 */
UENUM()
enum class EBuildSpatialMobility : uint8
{
	/** 稳定 Bounds，可在重建时进入不可变 Snapshot。 */
	Static,

	/** Bounds 可持续变化，永久保留在 Dynamic AABB Tree。 */
	Dynamic
};

/** Sparse Chunk 与 Dynamic AABB Tree 的纯数据配置。长度单位使用 UE 世界单位。 */
struct ELEMENTSANDBOXBUILDING_API FBuildSpatialIndexConfig final
{
	double ChunkSize = 5000.0;
	double DynamicBoundsPadding = 25.0;
	int32 MaxChunksPerEntity = 64;

	int32 AsyncSnapshotMinimumStaticEntries = 64;
	int32 AsyncSnapshotMinimumDeltaEntries = 64;
	double AsyncSnapshotDeltaRatio = 0.25;
	int32 AsyncSnapshotMinimumTombstones = 32;
	double AsyncSnapshotTombstoneRatio = 0.125;
	double AsyncSnapshotIdleSeconds = 0.5;
	int32 AsyncSnapshotMaxCapturesPerTick = 1;
	int32 AsyncSnapshotMaxPublishesPerTick = 1;
	int32 AsyncSnapshotMaxConcurrentBuilds = 2;
	int32 AsyncSnapshotMaxScheduleCandidatesPerTick = 64;

	bool IsValid() const;

	/** 将一个有限世界位置映射到 Sparse Grid Chunk；负坐标使用向下取整。 */
	bool TryGetChunkCoordinate(
		const FVector& WorldLocation,
		FIntVector& OutChunkCoordinate) const;
};

/** Building 空间索引的 Ray-AABB 命中；Distance 使用世界单位并按近到远排序。 */
struct ELEMENTSANDBOXBUILDING_API FBuildSpatialRayHit final
{
	FBuildEntityHandle Entity;
	double Distance = 0.0;
};

/**
 * Query 调用方持有并跨帧复用的临时内存。
 * Query 每次只清空元素，不主动收缩容量；同一个 Scratch 不允许并发使用。
 */
struct ELEMENTSANDBOXBUILDING_API FBuildSpatialQueryScratch final
{
	TArray<FBuildEntityHandle> Candidates;
	TSet<FBuildEntityHandle> UniqueEntities;
};

/** 单次 Game Thread Snapshot 调度的可观测结果。 */
struct ELEMENTSANDBOXBUILDING_API FBuildSpatialSnapshotWorkStats final
{
	int32 CapturedChunks = 0;
	int32 PublishedChunks = 0;
	int32 DiscardedStaleChunks = 0;
	int32 CheckedScheduleCandidates = 0;
};

/**
 * Building Entity 的独立空间索引。
 *
 * 每个 Sparse Chunk 组合静态 AABB Tree Snapshot、静态 Dynamic AABB Tree 增量区、
 * Snapshot Tombstone 和永久 Dynamic AABB Tree。所有结构修改和 Query 当前限定在
 * Game Thread；Overlap 顺序未定义，Ray 按距离和 Handle 确定排序，且跨 Chunk 的
 * 同一 Entity 只返回一次。
 *
 * 该类型不拥有 Entity 生命周期。调用方必须在销毁 Registry Entity 前后显式 Remove，
 * 空间索引不会反向依赖 Registry，也不会读取 Fragment。
 */
class ELEMENTSANDBOXBUILDING_API FBuildSpatialIndex final
{
public:
	explicit FBuildSpatialIndex(const FBuildSpatialIndexConfig& InConfig = FBuildSpatialIndexConfig());
	~FBuildSpatialIndex();

	FBuildSpatialIndex(const FBuildSpatialIndex&) = delete;
	FBuildSpatialIndex& operator=(const FBuildSpatialIndex&) = delete;
	FBuildSpatialIndex(FBuildSpatialIndex&&) = delete;
	FBuildSpatialIndex& operator=(FBuildSpatialIndex&&) = delete;

	bool Insert(
		FBuildEntityHandle Entity,
		const FBox& Bounds,
		EBuildSpatialMobility Mobility);
	void ReserveEntityCapacity(int32 EntityCapacity);
	bool Update(FBuildEntityHandle Entity, const FBox& Bounds);
	bool SetMobility(FBuildEntityHandle Entity, EBuildSpatialMobility Mobility);
	bool Remove(FBuildEntityHandle Entity);

	bool Contains(FBuildEntityHandle Entity) const;
	bool TryGetBounds(FBuildEntityHandle Entity, FBox& OutBounds) const;
	bool TryGetMobility(FBuildEntityHandle Entity, EBuildSpatialMobility& OutMobility) const;

	/** 清空 OutEntities，并返回与 QueryBounds 相交的去重 Entity。 */
	void QueryOverlaps(
		const FBox& QueryBounds,
		FBuildSpatialQueryScratch& Scratch,
		TArray<FBuildEntityHandle>& OutEntities) const;

	/**
	 * 沿有限世界射线遍历 Sparse Chunk 与 Chunk 内 AABB Tree。
	 * Direction 会在内部归一化；结果按 Bounds 首次命中距离稳定排序并跨 Chunk 去重。
	 */
	void QueryRay(
		const FVector& Origin,
		const FVector& Direction,
		double MaxDistance,
		FBuildSpatialQueryScratch& Scratch,
		TArray<FBuildSpatialRayHit>& OutHits) const;

	int32 GetEntityCount() const;
	/** Insert/Update/Mobility/Remove/Reset 任一查询可见变化都会推进。 */
	uint64 GetQueryRevision() const;
	int32 GetChunkCount() const;
	int32 GetDirtyStaticChunkCount() const;
	SIZE_T GetEstimatedAllocatedSize() const;
	int32 GetAsyncSnapshotInFlightCount() const;
	bool HasPendingAsyncSnapshotWork() const;

	/** 每次最多按 Config 捕获/发布；只允许在 Game Thread 调用。 */
	FBuildSpatialSnapshotWorkStats ProcessAsyncSnapshotWork();

	/**
	 * 将最多 MaxChunkCount 个 Dirty Chunk 的当前 Static 数据重建为只读 Snapshot。
	 * Persistent Dynamic Entity 不参与重建，也不会因为重建改变空间分类。
	 */
	int32 RebuildDirtyStaticChunks(int32 MaxChunkCount = MAX_int32);

	void Reset();

private:
	TUniquePtr<FBuildSpatialIndexData> Data;
};
