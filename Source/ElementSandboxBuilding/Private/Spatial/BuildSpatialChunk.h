#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Spatial/BuildAABBTree.h"
#include "Spatial/BuildDynamicAABBTree.h"

enum class EBuildSpatialMobility : uint8;

/** 单个 Sparse Chunk 的静态 Snapshot/Delta 与永久动态索引；仅由 Spatial Index 拥有。 */
class FBuildSpatialChunk final
{
public:
	explicit FBuildSpatialChunk(double DynamicBoundsPadding);

	bool Insert(
		FBuildEntityHandle Entity,
		const FBox& Bounds,
		EBuildSpatialMobility Mobility);
	bool Update(
		FBuildEntityHandle Entity,
		const FBox& Bounds,
		EBuildSpatialMobility Mobility);
	bool SetMobility(FBuildEntityHandle Entity, EBuildSpatialMobility Mobility);
	bool Remove(FBuildEntityHandle Entity, EBuildSpatialMobility Mobility);
	void Query(const FBox& QueryBounds, TArray<FBuildEntityHandle>& OutCandidates) const;
	void QueryRay(
		const FVector& Origin,
		const FVector& UnitDirection,
		double MaxDistance,
		TArray<FBuildEntityHandle>& OutCandidates) const;
	void RebuildStaticSnapshot();
	void CaptureStaticEntries(TArray<FBuildSpatialEntry>& OutEntries) const;
	bool PublishStaticSnapshot(uint64 Version, FBuildAABBTree&& NewSnapshot);

	uint64 GetStaticVersion() const { return StaticVersion; }
	int32 GetStaticCount() const { return StaticBounds.Num(); }
	int32 GetStaticSnapshotCount() const { return StaticSnapshot.Num(); }
	int32 GetStaticDeltaCount() const { return StaticDeltaTree.Num(); }
	int32 GetStaticTombstoneCount() const { return StaticSnapshotTombstones.Num(); }
	double GetLastStaticWriteTimeSeconds() const { return LastStaticWriteTimeSeconds; }
	SIZE_T GetAllocatedSize() const
	{
		return StaticBounds.GetAllocatedSize()
			+ DynamicBounds.GetAllocatedSize()
			+ StaticSnapshot.GetAllocatedSize()
			+ StaticDeltaTree.GetAllocatedSize()
			+ PersistentDynamicTree.GetAllocatedSize()
			+ StaticSnapshotTombstones.GetAllocatedSize();
	}

	bool IsStaticDirty() const
	{
		return StaticDeltaTree.Num() > 0 || !StaticSnapshotTombstones.IsEmpty();
	}

	bool IsEmpty() const
	{
		return StaticBounds.IsEmpty() && DynamicBounds.IsEmpty();
	}

private:
	void MarkStaticWrite();
	bool InsertStatic(FBuildEntityHandle Entity, const FBox& Bounds);
	bool InsertDynamic(FBuildEntityHandle Entity, const FBox& Bounds);
	bool UpdateStatic(FBuildEntityHandle Entity, const FBox& Bounds);
	bool UpdateDynamic(FBuildEntityHandle Entity, const FBox& Bounds);
	bool RemoveStatic(FBuildEntityHandle Entity);
	bool RemovePersistentDynamic(FBuildEntityHandle Entity);

	TMap<FBuildEntityHandle, FBox> StaticBounds;
	TMap<FBuildEntityHandle, FBox> DynamicBounds;
	FBuildAABBTree StaticSnapshot;
	FBuildDynamicAABBTree StaticDeltaTree;
	FBuildDynamicAABBTree PersistentDynamicTree;
	TSet<FBuildEntityHandle> StaticSnapshotTombstones;
	uint64 StaticVersion = 0;
	double LastStaticWriteTimeSeconds = 0.0;
};
