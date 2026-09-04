#pragma once

#include "CoreMinimal.h"
#include "Visual/ElementVisualShardKey.h"

/** 独立 Element 实例页的后端类型；不会映射到通用 MeshPool Layer。 */
enum class EElementVisualInstanceBackend : uint8
{
	Hierarchical,
	Instanced
};

/** 客户端 Element 显示的阈值、覆盖、Apply 预算与专用实例页配置。 */
struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementPresentationConfig final
{
	double ShardSize = FElementVisualShardKey::DefaultSize;
	double CoverageRadius = 25000.0;
	float HorizontalCoverageAngleDegrees = 180.0f;
	float VerticalCoverageAngleDegrees = 180.0f;
	double SubjectRecenterDistance = 1000.0;
	double ViewRecenterDistance = 1000.0;
	float FOVSafetyAngleDegrees = 10.0f;
	float MinimumRecenterAngleDegrees = 5.0f;
	float FieldOfViewChangeThresholdDegrees = 1.0f;
	float AspectRatioChangeThreshold = 0.01f;
	int32 ViewportDimensionChangeThreshold = 8;
	double GraceSeconds = 0.5;
	int32 MaxApplyCommandsPerTick = 1024;
	double MaxApplyMilliseconds = 4.0;
	int32 InstancesPerPage = 1024;
	int32 MaxSparePagesPerBackend = 8;
	int32 MaxCoverageShardsPerSource = 8192;

	bool IsValid() const;
	float CalculateHorizontalRecenterAngleDegrees(float CurrentHorizontalFOVDegrees) const;
	float CalculateVerticalRecenterAngleDegrees(float CurrentVerticalFOVDegrees) const;
};

/** 只读 Visual Source 注册句柄；Generation 防止旧注销请求命中新 Source。 */
struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementVisualSourceHandle final
{
public:
	FElementVisualSourceHandle() = default;
	bool IsSet() const { return Id != 0 && Generation != 0; }
	uint32 GetId() const { return Id; }
	uint32 GetGeneration() const { return Generation; }

	friend bool operator==(const FElementVisualSourceHandle& Left, const FElementVisualSourceHandle& Right)
	{
		return Left.Id == Right.Id && Left.Generation == Right.Generation;
	}

private:
	FElementVisualSourceHandle(uint32 InId, uint32 InGeneration)
		: Id(InId), Generation(InGeneration)
	{
	}

	uint32 Id = 0;
	uint32 Generation = 0;

	friend class UElementPresentationWorldSubsystem;
};

/** 累计计数与当前 Gauge；Gameplay 不得读取本结构改变调度行为。 */
struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementPresentationStats final
{
	int32 ViewSourceCount = 0;
	int32 CoveredShardCount = 0;
	int32 ResidentVisualCount = 0;
	int32 TargetVisualCount = 0;
	int32 AppliedVisualCount = 0;
	int32 InFlightBuildCount = 0;
	int32 PendingApplyCount = 0;
	int32 GracePendingShardCount = 0;
	int32 ActivePageCount = 0;
	int32 SparePageCount = 0;
	int32 HISMComponentCount = 0;
	int32 ISMComponentCount = 0;

	uint64 ViewUpdateCount = 0;
	uint64 ThresholdInvalidatedCount = 0;
	uint64 CoverageRecomputeCount = 0;
	uint64 CoverageAddCount = 0;
	uint64 CoverageRemoveCount = 0;
	uint64 CoverageUnchangedCount = 0;
	uint64 CoverageRefCountHitCount = 0;
	uint64 GraceScheduledCount = 0;
	uint64 GraceExpiredCount = 0;
	uint64 SnapshotReadCount = 0;
	uint64 JournalDeltaCount = 0;
	uint64 JournalDuplicateCount = 0;
	uint64 JournalGapCount = 0;
	uint64 JournalOverflowCount = 0;
	uint64 LocalRebuildCount = 0;
	uint64 GlobalRebuildCount = 0;
	uint64 BuildDispatchCount = 0;
	uint64 BuildCompleteCount = 0;
	uint64 BuildStaleDiscardCount = 0;
	uint64 BuildCoalescedCount = 0;
	uint64 BuildFailedCount = 0;
	uint64 PendingDeltaChaseCount = 0;
	uint64 ApplyCommandCount = 0;
	uint64 ApplyBudgetExhaustedCount = 0;
	uint64 ApplyFailedCount = 0;
	uint64 PoolPageAllocateCount = 0;
	uint64 PoolPageReuseCount = 0;
	uint64 IdleTickCount = 0;
	uint64 RepeatedSnapshotBuildCount = 0;
	uint64 InvalidVisibleRemovalCount = 0;
	uint64 DedicatedServerAllocationCount = 0;
};

struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementPresentationShardDebug final
{
	FElementVisualShardKey Shard;
	int32 CoverageRefCount = 0;
	int32 ResidentCount = 0;
	int32 TargetCount = 0;
	int32 AppliedCount = 0;
	uint64 ResidentCursor = 0;
	uint64 TargetRevision = 0;
	bool bInFlight = false;
	bool bGracePending = false;
};

struct ELEMENTSANDBOXELEMENTPRESENTATION_API FElementPresentationDebugSnapshot final
{
	TArray<FElementPresentationShardDebug> Shards;
	int32 PendingApplyCount = 0;
	bool bTickable = false;
};
