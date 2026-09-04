#pragma once

#include "CoreMinimal.h"

/** Building 对 MeshPool 后端的领域语义；物理 Cluster Key 由 Presentation 模块定义。 */
enum class EBuildRenderStorageClass : uint8
{
	StaticHISM,
	ColdPromotableHISM,
	HotISM
};

struct ELEMENTSANDBOXBUILDING_API FBuildRenderClusterConfig final
{
	/** 已驻留 HISM 的空间 Cluster Cell；与 50m Gameplay Chunk 无关。 */
	double StaticCellSize = 100000.0;

	bool IsValid() const
	{
		return FMath::IsFinite(StaticCellSize) && StaticCellSize > UE_SMALL_NUMBER;
	}
	bool TryGetCellCoordinate(const FVector& WorldLocation, FIntVector& OutCellCoordinate) const;
};

/** Building Projector 的自适应驻留口径；全部数值可由项目配置覆盖。 */
struct ELEMENTSANDBOXBUILDING_API FBuildPresentationResidencyConfig final
{
	/** 普通 Static Building 的 Packed BVH Cell；只索引当前 Resident Entity。 */
	double StaticCellSize = 100000.0;
	double MinimumLocalRadius = 10000.0;
	int32 LocalResidentTargetMeshParts = 300000;
	int32 StableResidentTargetMeshParts = 600000;
	int32 TransitionReserveMeshParts = 300000;
	int32 ResidentHardWatermarkMeshParts = 900000;
	int32 EmergencyOverflowMeshParts = 60000;
	double ForwardCoverageAngleDegrees = 180.0;
	double FOVSafetyAngleDegrees = 10.0;
	double MinimumRecenterAngleDegrees = 5.0;
	/** 同时约束镜头停稳与相关表现索引停稳，避免流式注入期间反复替换 Far 集合。 */
	double FarSettleSeconds = 0.20;
	double RapidRotationThresholdDegreesPerSecond = 180.0;
	double RotationReversalWindowSeconds = 1.0;
	double PromotionStableSeconds = 0.5;
	double UnstablePromotionLockSeconds = 2.0;
	int32 InitialMeshPoolWorkBudgetParts = 12000;
	int32 MinimumMeshPoolWorkBudgetParts = 2000;
	int32 MaximumMeshPoolWorkBudgetParts = 24000;
	int32 LocalTransitionPublishBudgetEntitiesPerCycle = 4096;
	double NormalInstanceApplyTargetMilliseconds = 4.0;
	double EmergencyInstanceApplyTargetMilliseconds = 6.0;
	double EvictionGraceSeconds = 5.0;
	double EvictionFrequencyHz = 2.0;
	double HotPromotionRadius = 10000.0;
	double SourceMovementThreshold = 2500.0;
	double GameplayChunkSize = 5000.0;
	double GameplayChunkPadding = 5000.0;
	bool IsValid() const
	{
				return FMath::IsFinite(StaticCellSize) && StaticCellSize > 0.0
					&& FMath::IsFinite(MinimumLocalRadius) && MinimumLocalRadius >= 0.0
					&& LocalResidentTargetMeshParts > 0
					&& StableResidentTargetMeshParts >= LocalResidentTargetMeshParts
					&& TransitionReserveMeshParts >= 0
					&& ResidentHardWatermarkMeshParts >= StableResidentTargetMeshParts
					&& EmergencyOverflowMeshParts >= 0
					&& FMath::IsFinite(ForwardCoverageAngleDegrees)
					&& ForwardCoverageAngleDegrees > 0.0 && ForwardCoverageAngleDegrees <= 360.0
					&& FMath::IsFinite(FOVSafetyAngleDegrees) && FOVSafetyAngleDegrees >= 0.0
					&& FMath::IsFinite(MinimumRecenterAngleDegrees) && MinimumRecenterAngleDegrees >= 0.0
					&& FMath::IsFinite(FarSettleSeconds) && FarSettleSeconds >= 0.0
					&& FMath::IsFinite(RapidRotationThresholdDegreesPerSecond)
					&& RapidRotationThresholdDegreesPerSecond > 0.0
					&& FMath::IsFinite(RotationReversalWindowSeconds) && RotationReversalWindowSeconds > 0.0
					&& FMath::IsFinite(PromotionStableSeconds) && PromotionStableSeconds >= 0.0
					&& FMath::IsFinite(UnstablePromotionLockSeconds) && UnstablePromotionLockSeconds >= 0.0
					&& MinimumMeshPoolWorkBudgetParts > 0
					&& InitialMeshPoolWorkBudgetParts >= MinimumMeshPoolWorkBudgetParts
					&& MaximumMeshPoolWorkBudgetParts >= InitialMeshPoolWorkBudgetParts
					&& LocalTransitionPublishBudgetEntitiesPerCycle > 0
					&& FMath::IsFinite(NormalInstanceApplyTargetMilliseconds)
					&& NormalInstanceApplyTargetMilliseconds > 0.0
					&& FMath::IsFinite(EmergencyInstanceApplyTargetMilliseconds)
					&& EmergencyInstanceApplyTargetMilliseconds >= NormalInstanceApplyTargetMilliseconds
					&& FMath::IsFinite(EvictionGraceSeconds) && EvictionGraceSeconds >= 0.0
					&& FMath::IsFinite(EvictionFrequencyHz) && EvictionFrequencyHz > 0.0
					&& FMath::IsFinite(HotPromotionRadius) && HotPromotionRadius >= 0.0
					&& FMath::IsFinite(SourceMovementThreshold) && SourceMovementThreshold >= 0.0
					&& FMath::IsFinite(GameplayChunkSize) && GameplayChunkSize > 0.0
				&& FMath::IsFinite(GameplayChunkPadding) && GameplayChunkPadding >= 0.0;
	}
};

enum class EBuildPresentationTransitionPhase : uint8
{
	Stable,
	RapidSettling,
	AsyncSelect,
	CatchUpVisible,
	ReclaimOld,
	FillTarget,
	CommitCleanup
};

struct ELEMENTSANDBOXBUILDING_API FBuildPresentationSelectionStats final
{
	int32 RequiredEntityCount = 0;
	int32 RequiredMeshPartCount = 0;
	int32 CachedOnlyEntityCount = 0;
	int32 CachedOnlyMeshPartCount = 0;
	int32 ResidentEntityCount = 0;
	int32 ResidentMeshPartCount = 0;
	double LocalResidentBoundary = 0.0;
	int32 EvictionCandidateCount = 0;
	int32 EvictionGraceBlockedCount = 0;
	int32 LastEvictedEntityCount = 0;
	int32 LastEvictedMeshPartCount = 0;
	int32 CandidateNodeCount = 0;
	int32 CandidateEntryCount = 0;
	int32 PrunedNodeCount = 0;
	int32 AcceptedSubtreeCount = 0;
	int32 LocalSelectionCacheHitSourceCount = 0;
	int32 LastAddedResidentEntityCount = 0;
	int32 LastAddedResidentMeshPartCount = 0;
	int32 PendingRequiredEntityCount = 0;
	int32 PendingRequiredMeshPartCount = 0;
	int32 TransitionLocalMeshPartCount = 0;
	int32 PendingLocalPreparationEntityCount = 0;
	int32 PendingLocalRenderEntityCount = 0;
	int32 PendingLocalReleaseEntityCount = 0;
	int32 HotPinnedEntityCount = 0;
	int32 HotPinMaintenanceEntityCount = 0;
	int32 ActiveFarMeshPartCount = 0;
	int32 TransitionFarMeshPartCount = 0;
	int32 OverlappingFarMeshPartCount = 0;
	int32 VisibleCoreMissingMeshPartCount = 0;
	int32 AsyncSelectionInFlightCount = 0;
	int32 LocalAsyncSelectionInFlightCount = 0;
	uint64 StaleAsyncResultCount = 0;
	int32 RapidRotationFrozenSourceCount = 0;
	EBuildPresentationTransitionPhase TransitionPhase = EBuildPresentationTransitionPhase::Stable;
	int32 CurrentMeshPoolWorkBudgetParts = 0;
	int32 LastCycleAddedMeshPartCount = 0;
	int32 LastCycleRemovedMeshPartCount = 0;
	double LastSelectionMilliseconds = 0.0;
	double LastProjectionMilliseconds = 0.0;
	uint64 SelectionPassCount = 0;
	uint64 LocalSelectionPassCount = 0;
	int32 StaticCellCount = 0;
	int32 MutableChunkCount = 0;
	int64 StaticPackedEntryCount = 0;
	int64 StaticDeltaEntryCount = 0;
	uint64 StaticBVHBuildCount = 0;
	uint64 IndexRevision = 0;
};
