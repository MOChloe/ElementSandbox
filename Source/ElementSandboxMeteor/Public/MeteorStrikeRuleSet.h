#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeteorRuntimeTypes.h"

#include "MeteorStrikeRuleSet.generated.h"

/** 陨石 Runtime 唯一策划配置；Subsystem 初始化时冻结为 FMeteorRuntimeConfig。 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXMETEOR_API UMeteorStrikeRuleSet final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UMeteorStrikeRuleSet();
	UE::ElementSandbox::Meteor::FMeteorRuntimeConfig Freeze() const;

	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="100.0"))
	float MeteorHeight = 600000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="100.0"))
	float MeteorDiameter = 300000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="0.1"))
	float MeteorFallSeconds = 6.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="10000.0"))
	float MeteorShowcaseMinimumDistance = 10000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="10000.0"))
	float MeteorShowcasePreferredDistance = 20000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="10000.0"))
	float MeteorShowcaseMaximumDistance = 30000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Strike", meta=(ClampMin="10000.0"))
	float MeteorApproachHorizontalDistance = 400000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Shockwave", meta=(ClampMin="100.0"))
        float ShockwaveRadius = 600000.0f;
        UPROPERTY(EditDefaultsOnly, Category="Meteor|Shockwave", meta=(ClampMin="100.0"))
	float ImpactCoreRadius = 200000.0f;
        UPROPERTY(EditDefaultsOnly, Category="Meteor|Shockwave", meta=(ClampMin="1.0"))
	float ShockwaveSpeed = 300000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0"))
	float RadialStrength = 18000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0"))
	float UpwardStrength = 14000.0f;
	/** 解析碎片速度（cm/s）；实际方向由外向半平面与分层仰角共同决定。 */
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisSpeedRange = FVector2D(9000.0, 22000.0);
	/** 仰角层：中高弧线构成主体，让碎块明显升空、滞空并回落。 */
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisLowElevationDegrees = FVector2D(8.0, 28.0);
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisMediumElevationDegrees = FVector2D(28.0, 50.0);
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisHighElevationDegrees = FVector2D(50.0, 72.0);
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0", ClampMax="1.0"))
	float DebrisMediumArcFraction = 0.42f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0", ClampMax="1.0"))
	float DebrisHighArcFraction = 0.18f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0", ClampMax="89.0"))
	float DebrisAirMaximumAzimuthDeviationDegrees = 86.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0", ClampMax="89.0"))
	float DebrisGroundMaximumAzimuthDeviationDegrees = 88.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.0", ClampMax="1.0"))
	float DebrisGroundScatterFraction = 0.18f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisGroundElevationDegrees = FVector2D(-8.0, 12.0);
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	FVector2D DebrisGroundSpeedMultiplier = FVector2D(0.35, 0.65);
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMax="-1.0"))
	float GravityZ = -1600.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris")
	float GroundPlaneZ = 0.0f;
	/** 首次触地后继续由解析 GPU Lane 完成翻滚、躺平，不为大批碎片创建 Chaos Body。 */
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Debris", meta=(ClampMin="0.1", ClampMax="2.0"))
	float DebrisSettlingSeconds = 0.55f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Network", meta=(ClampMin="0.1"))
	float NetworkLeadSeconds = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Network", meta=(ClampMin="0.0"))
	float EncodingEstimateSeconds = 0.10f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="0.0"))
	float QueueSafetySeconds = 0.35f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="8"))
	int32 MaximumWorkPages = 512;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="1"))
	int32 MaximumQueryTilesPerPump = 64;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="1"))
	int32 MaximumDestructionTargetsPerPump = 1024;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="1", ClampMax="1024"))
	int32 MaximumDestructionTargetsPerSnapshotBatch = 32;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="0.05"))
	float LocalServerGameplayBudgetMilliseconds = 3.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="0.05"))
	float DedicatedServerGameplayBudgetMilliseconds = 8.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="1"))
	int32 LocalServerWorkerConcurrency = 1;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Scheduling", meta=(ClampMin="1"))
	int32 DedicatedServerWorkerConcurrency = 4;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Settlement", meta=(ClampMin="1"))
	int32 MaximumSettlementBatchSize = 128;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Settlement", meta=(ClampMin="100.0"))
	float SettlementPriorityCellSize = 5000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Settlement", meta=(ClampMin="100.0"))
	float SettlementPriorityRadius = 30000.0f;
	UPROPERTY(EditDefaultsOnly, Category="Meteor|Settlement", meta=(ClampMin="0"))
	int32 MinimumGlobalSettlementCountPerBatch = 16;
		UPROPERTY(EditDefaultsOnly, Category="Meteor|Client", meta=(ClampMin="0.1"))
		float ClientGameThreadBudgetMilliseconds = 1.5f;
};
