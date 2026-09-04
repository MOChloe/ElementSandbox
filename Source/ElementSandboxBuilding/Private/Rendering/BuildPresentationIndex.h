#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Rendering/BuildPresentationResidencySelector.h"
#include "Rendering/BuildRenderTypes.h"
#include "Storage/BuildStableArrayAllocator.h"

bool TryGetPresentationGridCoordinate(const FVector& WorldLocation, double CellSize, FIntVector& OutCoordinate);

struct FPresentationEntry final
{
	FBuildEntityHandle Entity;
	FBox Bounds = FBox(ForceInit);
	int32 MeshPartCost = 0;
	int32 DynamicPartCost = 0;
	FIntVector Cell = FIntVector::ZeroValue;
	int32 IndexInCell = INDEX_NONE;
	int32 StaticSnapshotSerial = INDEX_NONE;
	bool bPackedStatic = false;
};

using FPresentationSelectorEntryBlock = TArray<FBuildPresentationSelectorEntry>;

class FBuildPresentationPackedAsyncState;

struct FPresentationStaticCell final
{
	static constexpr int32 MinimumPackedRebuildDeltaEntries = 256;

	TArray<int32> EntrySlots;
	FBox Bounds = FBox(ForceInit);
	FBox DeltaBounds = FBox(ForceInit);
	int32 EntityCount = 0;
	int32 MeshPartCount = 0;
	int32 DynamicPartCount = 0;
	int32 PackedEntryCount = 0;
	int32 DeltaMeshPartCost = 0;
	int32 DeltaMinimumMeshPartCost = MAX_int32;
	SIZE_T EstimatedBytes = 0;
	uint64 Revision = 1;
	bool bTreeDirty = false;
	TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe> PublishedSnapshot;
	/** 当前 Resident Static Entity 的不可变纯值块；请求只复制共享指针，不复制全量 Entry。 */
	TArray<TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>> PendingDeltaBlocks;
	TArray<TSharedPtr<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe>> InFlightDeltaBlocks;
	int32 PendingDeltaEntryCount = 0;
	int32 InFlightDeltaEntryCount = 0;
	/** 自上次成功 Packed Build 后尚未压实的旧快照墓碑数。 */
	int32 TombstonedEntryCount = 0;
	uint64 PackedBuildId = 0;
	uint64 StructuralRevision = 1;
	bool bPackedBuildInFlight = false;

	int32 GetDeltaEntryCount() const { return FMath::Max(0, EntrySlots.Num() - PackedEntryCount); }
};

struct FPresentationMutableChunk final
{
	TArray<int32> EntrySlots;
	FBox Bounds = FBox(ForceInit);
	int32 EntityCount = 0;
	int32 MeshPartCount = 0;
	int32 DynamicPartCount = 0;
	int32 MinimumMeshPartCost = MAX_int32;
	SIZE_T EstimatedBytes = 0;
	uint64 Revision = 1;
};

struct FBuildPresentationIndexStats final
{
	SIZE_T EstimatedAllocatedSize = 0;
	int32 StaticCellCount = 0;
	int32 MutableChunkCount = 0;
	int32 StaticPackedEntryCount = 0;
	int32 StaticDeltaEntryCount = 0;
	uint64 StaticBVHBuildCount = 0;
	uint64 IndexRevision = 0;
};

struct FPresentationViewSource;

/**
 * Building 表现专用空间索引。
 *
 * 它拥有 1km Resident Static Cell、不可变 Packed BVH Snapshot、每 Cell 有界 Delta 以及
 * 普通可变 50m Chunk；不持有 Registry、World、UObject 或 MeshPool 状态。
 */
class FBuildPresentationIndex
{
public:
	explicit FBuildPresentationIndex(const FBuildPresentationResidencyConfig& Config);
	~FBuildPresentationIndex();

	FPresentationEntry* FindEntry(FBuildEntityHandle Entity);
	const FPresentationEntry* FindEntry(FBuildEntityHandle Entity) const;
	bool UpsertEntry(FBuildEntityHandle Entity, const FBox& Bounds, int32 MeshPartCost, int32 DynamicPartCost,
					 bool bPackedStatic);
	void RemoveEntry(FBuildEntityHandle Entity);
	void Clear();
	void ReserveEntityCapacity(int32 EntityCapacity);
	/** 发布已完成结果并至多调度一个纯数据 Packed BVH Build；生产路径不在 Game Thread Build。 */
	void ProcessAsyncPackedBuildWork(bool bBuildSynchronouslyForTesting = false);
	bool HasPendingAsyncPackedBuildWork() const;

	uint64 GetIndexRevision() const { return IndexRevision; }
	uint64 GetStaticBVHBuildCount() const { return StaticBVHBuildCount; }
	/** 固化 Worker 只读请求；调用方不直接遍历索引内部容器。 */
	void CaptureSelectionSources(FBuildLocalSelectionRequest& OutRequest) const;
	void CaptureSelectionSources(FBuildFarSelectionRequest& OutRequest) const;
	/** 收集任一观察主体 Hot 半径内的实体，不暴露 Entry 存储布局。 */
	void GatherHotPinnedEntities(TConstArrayView<FPresentationViewSource> Views, double Radius,
								 TSet<FBuildEntityHandle>& OutEntities) const;
	FBuildPresentationIndexStats GetStats() const;

private:
	template <typename RequestType> void CaptureSelectionSourcesImpl(RequestType& OutRequest) const;

	TArray<FPresentationEntry, FBuildStableArrayAllocator> EntriesBySlot;
	TMap<FIntVector, FPresentationStaticCell> StaticCells;
	TMap<FIntVector, FPresentationMutableChunk> MutableChunks;
	uint64 IndexRevision = 1;
	uint64 StaticBVHBuildCount = 0;
	void BumpIndexRevision();
	void AppendStaticDeltaBlock(FPresentationStaticCell& Cell,
								TSharedRef<const FPresentationSelectorEntryBlock, ESPMode::ThreadSafe> Block);
	TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> GetStaticEntryTombstoneSnapshot() const;
	void ResetPackedAsyncState();

	double StaticCellSize = 100000.0;
	double GameplayChunkSize = 5000.0;
	/** Serial 直接作为 bit index；删除只置一位，内存约为历史 Packed Entry 数 / 8。 */
	TBitArray<> StaticEntryTombstones;
	mutable TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> PublishedStaticEntryTombstones;
	mutable bool bStaticEntryTombstoneSnapshotDirty = false;
	TSharedPtr<FBuildPresentationPackedAsyncState, ESPMode::ThreadSafe> PackedAsyncState;
};
