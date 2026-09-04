#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"

struct FBuildPresentationSelectorEntry final
{
	FBuildEntityHandle Entity;
	FBox Bounds = FBox(ForceInit);
	int32 MeshPartCost = 0;
	double LastRequiredTimeSeconds = 0.0;
	/**
	 * Packed Static Entry 的不可复用版本序号。Mutable Entry 使用 INDEX_NONE。
	 * Worker 通过请求内的只读墓碑位图跳过旧快照版本，Entity 更新后的新版本不会被旧墓碑误杀。
	 */
	int32 StaticSnapshotSerial = INDEX_NONE;
};

struct FBuildPresentationSelectorNode final
{
	FBox Bounds = FBox(ForceInit);
	int32 LeftChild = INDEX_NONE;
	int32 RightChild = INDEX_NONE;
	int32 FirstEntry = 0;
	int32 EntryCount = 0;
	int32 MeshPartCost = 0;
	int32 MinimumMeshPartCost = MAX_int32;

	bool IsLeaf() const { return LeftChild == INDEX_NONE; }
};

struct FBuildPresentationSectorVisibility final
{
	bool bIntersects = false;
	bool bFullyInside = false;
	double Score = TNumericLimits<double>::Max();
};

/** Static Cell 发布给后台任务的不可变值快照。 */
struct FBuildPresentationCellSnapshot final
{
	TArray<FBuildPresentationSelectorEntry> OrderedEntries;
	TArray<FBuildPresentationSelectorNode> Nodes;
	uint64 Revision = 0;
};

struct FBuildLocalSelectionRequest final
{
	uint64 SourceToken = 0;
	uint64 RequestId = 0;
	/** 只由该 Source 已订阅的 Local Cell 变化推进。 */
	uint64 SourceRevision = 0;
	/** 请求固化时的全局 Local Cell 变更序号，用于检查 Worker 在途期间的新变化。 */
	uint64 IndexChangeSerial = 0;
	FVector SubjectLocation = FVector::ZeroVector;
	double MinimumLocalRadius = 0.0;
	double HotPromotionRadius = 0.0;
	int32 TargetLocalMeshParts = 0;
	TArray<TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>> StaticCells;
	TArray<TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>> StaticDeltaBlocks;
	/** Packed Static 旧快照版本的只读墓碑；一次请求只共享指针，不复制 Cell 或 Entry。 */
	TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> StaticEntryTombstones;
	TArray<FBuildPresentationSelectorEntry> DeltaEntries;
	TArray<FBuildPresentationSelectorEntry> MutableEntries;
};

struct FBuildLocalSelectionResult final
{
	uint64 SourceToken = 0;
	uint64 RequestId = 0;
	uint64 SourceRevision = 0;
	uint64 IndexChangeSerial = 0;
	FVector SubjectLocation = FVector::ZeroVector;
	/** Worker 直接发布 Handle 集合；Game Thread 不再为全量结果重建一次 TSet。 */
	TArray<FBuildEntityHandle> OrderedTargetEntities;
	TSet<FBuildEntityHandle> TargetEntities;
	/** 在 Worker 上复制一次，Far 请求只共享指针，不在 Game Thread 复制 30 万排除项。 */
	TSharedPtr<const TSet<FBuildEntityHandle>, ESPMode::ThreadSafe> TargetEntitiesSnapshot;
	TSet<FBuildEntityHandle> HotPinnedEntities;
	int32 TargetMeshPartCost = 0;
	double Boundary = 0.0;
	int32 CandidateNodeCount = 0;
	int32 CandidateEntryCount = 0;
	int32 PrunedNodeCount = 0;
};

struct FBuildFarSelectionRequest final
{
	uint64 SourceToken = 0;
	uint64 RequestId = 0;
	/** 只由该 Source 已订阅的 Far Cell 或 Local 排除集合变化推进。 */
	uint64 SourceRevision = 0;
	uint64 IndexChangeSerial = 0;
	FVector ViewLocation = FVector::ZeroVector;
	FVector SubjectLocation = FVector::ZeroVector;
	FVector2D Forward = FVector2D(1.0, 0.0);
	double HorizontalFOVDegrees = 90.0;
	double CoverageAngleDegrees = 180.0;
	double FOVSafetyAngleDegrees = 10.0;
	int32 TargetFarMeshParts = 0;
	TArray<TSharedPtr<const FBuildPresentationCellSnapshot, ESPMode::ThreadSafe>> StaticCells;
	TArray<TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe>> StaticDeltaBlocks;
	TSharedPtr<const TBitArray<>, ESPMode::ThreadSafe> StaticEntryTombstones;
	TArray<FBuildPresentationSelectorEntry> DeltaEntries;
	TArray<FBuildPresentationSelectorEntry> MutableEntries;
	TSharedPtr<const TSet<FBuildEntityHandle>, ESPMode::ThreadSafe> LocalExclusions;
	TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe> ExistingActiveEntries;
	/**
	 * 上一份尚在接管中的 Far 目标快照。Worker 用它与新 TargetSet 求精确差集，
	 * 避免结果到达后 GameThread 再扫描整份 TransitionFarSet。
	 */
	TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe> ExistingTransitionEntries;
	TArray<FVector> AllSubjectLocations;
};

struct FBuildFarSelectionResult final
{
	uint64 SourceToken = 0;
	uint64 RequestId = 0;
	uint64 SourceRevision = 0;
	uint64 IndexChangeSerial = 0;
	FVector ViewLocation = FVector::ZeroVector;
	FVector SubjectLocation = FVector::ZeroVector;
	FVector2D Forward = FVector2D(1.0, 0.0);
	double HorizontalFOVDegrees = 90.0;
	double CoverageAngleDegrees = 180.0;
	TArray<FBuildPresentationSelectorEntry> OrderedTargetEntries;
	/** Select Worker 已经为去重维护了该集合；随结果转移，GameThread 不得再按全部目标重建。 */
	TSet<FBuildEntityHandle> TargetEntities;
	/** Worker 侧快照，晋升后可供下一次 Far 请求 O(1) 共享。 */
	TSharedPtr<const TArray<FBuildPresentationSelectorEntry>, ESPMode::ThreadSafe> OrderedTargetEntriesSnapshot;
	TArray<FBuildEntityHandle> ReclaimOrder;
	/** Worker 已求出的旧 Transition - 新 Target；GameThread 只按预算消费该顺序日志。 */
	TArray<FBuildEntityHandle> SupersededTransitionEntities;
	int32 VisibleCoreEntryCount = 0;
	int32 VisibleCoreMeshPartCost = 0;
	int32 TargetMeshPartCost = 0;
	int32 RequestedMeshPartCost = 0;
	/** 已接收目标的最大扇区优先级；用于 Source 订阅只命中可能改写目标的 Cell。 */
	double BoundaryScore = 0.0;
	int32 CandidateNodeCount = 0;
	int32 CandidateEntryCount = 0;
	int32 PrunedNodeCount = 0;
	int32 AcceptedSubtreeCount = 0;
};

/** 只读纯值选择器；可安全在线程池执行，不访问 Registry、World、UObject 或 MeshPool。 */
class FBuildPresentationResidencySelector final
{
public:
	static FBuildPresentationSectorVisibility EvaluateSectorBounds(
		const FBox& Bounds,
		const FVector& ViewLocation,
		const FVector& SubjectLocation,
		const FVector2D& UnitForward,
		double HalfAngleDegrees);
	static FBuildLocalSelectionResult SelectLocal(FBuildLocalSelectionRequest&& Request);
	static FBuildFarSelectionResult Select(FBuildFarSelectionRequest&& Request);
};
