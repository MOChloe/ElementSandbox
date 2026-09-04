#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BuildPresentationSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Building Presentation"))
class ELEMENTSANDBOXBUILDING_API UBuildPresentationSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0.0", Units="cm"))
	double MinimumLocalRadius = 10000.0;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="1"))
	int32 LocalResidentTargetMeshParts = 300000;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="1"))
	int32 StableResidentTargetMeshParts = 600000;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0"))
	int32 TransitionReserveMeshParts = 300000;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="1"))
	int32 ResidentHardWatermarkMeshParts = 900000;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0"))
	int32 EmergencyOverflowMeshParts = 60000;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0.1", ClampMax="360.0", Units="deg"))
	double ForwardCoverageAngleDegrees = 180.0;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0.0", ClampMax="180.0", Units="deg"))
	double FOVSafetyAngleDegrees = 10.0;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.0", ClampMax="180.0", Units="deg"))
	double MinimumRecenterAngleDegrees = 5.0;

	/** 观察方向或相关表现索引最后一次变化后，Far 选择至少等待多久再发布。 */
	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.0", Units="s"))
	double FarSettleSeconds = 0.20;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="1.0", Units="deg/s"))
	double RapidRotationThresholdDegreesPerSecond = 180.0;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.01", Units="s"))
	double RotationReversalWindowSeconds = 1.0;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.0", Units="s"))
	double PromotionStableSeconds = 0.5;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.0", Units="s"))
	double UnstablePromotionLockSeconds = 2.0;

	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="1"))
	int32 InitialMeshPoolWorkBudgetParts = 12000;

	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="1"))
	int32 MinimumMeshPoolWorkBudgetParts = 2000;

	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="1"))
	int32 MaximumMeshPoolWorkBudgetParts = 24000;

	/** Worker 返回的 Local 选择结果每个客户端投影周期最多在 Game Thread 发布多少个 Entity。 */
	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="1"))
	int32 LocalTransitionPublishBudgetEntitiesPerCycle = 4096;

	/** 普通周期的同步 Residency 实例变更墙钟上限，同时作为 MeshPool Flush 自适应反馈目标。 */
	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="0.01", Units="ms"))
	double NormalInstanceApplyTargetMilliseconds = 4.0;

	/** 可见缺口追赶周期允许的同步 Residency 实例变更墙钟上限。 */
	UPROPERTY(Config, EditAnywhere, Category="Work Budget", meta=(ClampMin="0.01", Units="ms"))
	double EmergencyInstanceApplyTargetMilliseconds = 6.0;

	UPROPERTY(Config, EditAnywhere, Category="Eviction", meta=(ClampMin="0.0", Units="s"))
	double EvictionGraceSeconds = 5.0;

	UPROPERTY(Config, EditAnywhere, Category="Eviction", meta=(ClampMin="0.01", Units="Hz"))
	double EvictionFrequencyHz = 2.0;

	UPROPERTY(Config, EditAnywhere, Category="Residency", meta=(ClampMin="0.0", Units="cm"))
	double HotPromotionRadius = 10000.0;

	UPROPERTY(Config, EditAnywhere, Category="Source Threshold", meta=(ClampMin="0.0", Units="cm"))
	double SourceMovementThreshold = 2500.0;

	UPROPERTY(Config, EditAnywhere, Category="Index", meta=(ClampMin="100.0", Units="cm"))
	double StaticCellSize = 100000.0;

	UPROPERTY(Config, EditAnywhere, Category="Index", meta=(ClampMin="100.0", Units="cm"))
	double GameplayChunkSize = 5000.0;

	/** 节点级查询的边界填充，避免 Chunk 边缘漏选。 */
	UPROPERTY(Config, EditAnywhere, Category="Index", meta=(ClampMin="0.0", Units="cm"))
	double GameplayChunkPadding = 5000.0;
};
