#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "SettlementTreeSettings.generated.h"

/** 树木表现与碰撞的独立预算；不读取或镜像 Building 配置。 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Settlement Tree Settings"))
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreeSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "1000.0", Units = "cm"))
	double TreeCellSize = 100000.0;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "cm"))
	double LocalRadius = 10000.0;
	UPROPERTY(Config, EditAnywhere, Category = "Selection",
			  meta = (ClampMin = "1.0", ClampMax = "360.0", Units = "deg"))
	float ForwardCoverageAngleDegrees = 360.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection",
			  meta = (ClampMin = "0.0", ClampMax = "180.0", Units = "deg"))
	float FOVSafetyAngleDegrees = 10.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection",
			  meta = (ClampMin = "0.0", ClampMax = "180.0", Units = "deg"))
	float MinimumRecenterAngleDegrees = 5.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "cm"))
	double SourceMovementThreshold = 2500.0;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "deg/s"))
	float RapidRotationThresholdDegreesPerSecond = 180.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "s"))
	float RapidRotationSettleSeconds = 0.2f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "s"))
	float GraceSeconds = 5.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "s"))
	float PromotionStableSeconds = 0.5f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.01", Units = "s"))
	float RotationReversalWindowSeconds = 1.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0", Units = "s"))
	float UnstablePromotionLockSeconds = 2.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 StableInstanceBudget = 128000;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0"))
	int32 TransitionReserve = 96000;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0"))
	int32 EmergencyReserve = 16000;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 AbsoluteHardMax = 240000;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 InitialChangesPerCycle = 16384;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 MinimumChangesPerCycle = 4096;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 MaximumChangesPerCycle = 32768;
	/** 单次不可抢占的原生 HISM Add/Remove 上限；不是每帧总吞吐上限。 */
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 MaximumNativeInstanceBatchSize = 2048;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0.1", Units = "ms"))
	double InstanceApplyTargetMilliseconds = 2.0;
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0.1", Units = "ms"))
	double InstanceApplyHardMilliseconds = 3.5;
	UPROPERTY(Config, EditAnywhere, Category = "HISM", meta = (ClampMin = "0.0", Units = "s"))
	double TreeBuildQuietSeconds = 0.25;
	UPROPERTY(Config, EditAnywhere, Category = "HISM", meta = (ClampMin = "0.0", Units = "s"))
	double TreeBuildMaxDeferralSeconds = 1.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "cm"))
	double ImmediateCollisionRadius = 400.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "cm"))
	double CameraCorridorPadding = 100.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "s"))
	double PredictionSeconds = 1.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "cm"))
	double RetentionPadding = 300.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "s"))
	double CollisionGraceSeconds = 3.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	int32 PredictiveAddsPerFrame = 16;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	int32 RemovesPerFrame = 32;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "cm"))
	double CollisionSourceMoveThreshold = 50.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "deg"))
	double CollisionSourceDirectionThresholdDegrees = 15.0;
	UPROPERTY(Config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0.0", Units = "cm/s"))
	double CollisionSourceSpeedThreshold = 100.0;
};
