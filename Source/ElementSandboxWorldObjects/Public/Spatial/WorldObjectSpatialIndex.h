#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectEntityHandle.h"
#include "Entity/WorldObjectTypes.h"

struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectSpatialConfig final
{
	double DynamicBoundsPadding = 20.0;
	/** PermanentStatic 按当前 Resident HomeChunk 分片；默认与 WorldStorage 的 100m Chunk 对齐。 */
	double StaticChunkSize = 10000.0;
};

struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectSpatialRayHit final
{
	FWorldObjectEntityHandle Entity;
	double Distance = 0.0;
};

/** 查询调用方长期复用，避免 Focus 等逐帧路径反复申请遍历栈。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectSpatialQueryScratch final
{
	TArray<int32> NodeStack;
	TArray<FIntVector> StaticChunkCandidates;
	TSet<FIntVector> VisitedStaticChunks;
	int32 LastVisitedStaticNodes = 0;
	int32 LastVisitedDynamicNodes = 0;

	int32 GetLastVisitedNodeCount() const
	{
		return LastVisitedStaticNodes + LastVisitedDynamicNodes;
	}
};

class FWorldObjectSpatialIndexImpl;

/**
 * 场景物件自己的双索引：PermanentStatic 使用 100m 坐标哈希目录；Chunk 内不超过
 * 32 条时连续扫描，超过后使用小 BVH。Portable 一生位于 Dynamic AABB Tree。
 */
class ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectSpatialIndex final
{
public:
	explicit FWorldObjectSpatialIndex(
		const FWorldObjectSpatialConfig& InConfig = FWorldObjectSpatialConfig());
	~FWorldObjectSpatialIndex();

	FWorldObjectSpatialIndex(const FWorldObjectSpatialIndex&) = delete;
	FWorldObjectSpatialIndex& operator=(const FWorldObjectSpatialIndex&) = delete;

	bool Insert(
		FWorldObjectEntityHandle Entity,
		const FBox& WorldBounds,
		EWorldObjectSpatialClass SpatialClass);
	bool Insert(
		FWorldObjectEntityHandle Entity,
		const FBox& WorldBounds,
		EWorldObjectSpatialClass SpatialClass,
		const FVector& HomeAnchorLocation);
	bool Remove(FWorldObjectEntityHandle Entity);

	/** PermanentStatic 更新会被拒绝；Portable 仅在越出 Fat Bounds 时重插。 */
	bool UpdatePortable(FWorldObjectEntityHandle Entity, const FBox& WorldBounds);

	bool Contains(FWorldObjectEntityHandle Entity) const;
	bool TryGetBounds(FWorldObjectEntityHandle Entity, FBox& OutBounds) const;
	bool TryGetSpatialClass(
		FWorldObjectEntityHandle Entity,
		EWorldObjectSpatialClass& OutClass) const;

	void QueryOverlaps(
		const FBox& QueryBounds,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectEntityHandle>& OutEntities) const;
	/** 只查询 Portable Dynamic Tree，不触发 Static BVH 构建。 */
	void QueryPortableOverlaps(
		const FBox& QueryBounds,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectEntityHandle>& OutEntities) const;
	void QueryRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectSpatialRayHit>& OutHits) const;
	/** 只查询 Portable Dynamic Tree，供拾取等已知类别的热路径使用。 */
	void QueryPortableRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		FWorldObjectSpatialQueryScratch& Scratch,
		TArray<FWorldObjectSpatialRayHit>& OutHits) const;

	/** 只提交受影响的 Chunk；不会重建全 Resident 顶层目录。 */
	bool RebuildStaticIfDirty();
	bool IsStaticDirty() const;
	int32 GetEntityCount() const;
	int32 GetPermanentStaticCount() const;
	int32 GetPortableCount() const;
	uint64 GetStaticBuildCount() const;
	int32 GetStaticLinearChunkCount() const;
	int32 GetStaticBVHChunkCount() const;
	uint64 GetDynamicReinsertCount() const;
	SIZE_T GetEstimatedAllocatedSize() const;
	bool ValidateDynamicTree() const;

private:
	TUniquePtr<FWorldObjectSpatialIndexImpl> Impl;
};
