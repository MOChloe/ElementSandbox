#include "MeteorStrikeRuleSet.h"

UMeteorStrikeRuleSet::UMeteorStrikeRuleSet() = default;

UE::ElementSandbox::Meteor::FMeteorRuntimeConfig UMeteorStrikeRuleSet::Freeze() const
{
	UE::ElementSandbox::Meteor::FMeteorRuntimeConfig Result;
	Result.MeteorHeight = MeteorHeight;
	Result.MeteorDiameter = MeteorDiameter;
	Result.MeteorFallSeconds = MeteorFallSeconds;
	Result.MeteorShowcaseMinimumDistance = MeteorShowcaseMinimumDistance;
	Result.MeteorShowcasePreferredDistance = MeteorShowcasePreferredDistance;
	Result.MeteorShowcaseMaximumDistance = MeteorShowcaseMaximumDistance;
	Result.MeteorApproachHorizontalDistance = MeteorApproachHorizontalDistance;
	Result.ShockwaveRadius = ShockwaveRadius;
	Result.ImpactCoreRadius = ImpactCoreRadius;
	Result.ShockwaveSpeed = ShockwaveSpeed;
	Result.RadialStrength = RadialStrength;
	Result.UpwardStrength = UpwardStrength;
	Result.DebrisSpeedRange = FVector2f(DebrisSpeedRange);
	Result.DebrisLowElevationDegrees = FVector2f(DebrisLowElevationDegrees);
	Result.DebrisMediumElevationDegrees = FVector2f(DebrisMediumElevationDegrees);
	Result.DebrisHighElevationDegrees = FVector2f(DebrisHighElevationDegrees);
	Result.DebrisMediumArcFraction = DebrisMediumArcFraction;
	Result.DebrisHighArcFraction = DebrisHighArcFraction;
	Result.DebrisAirMaximumAzimuthDeviationDegrees = DebrisAirMaximumAzimuthDeviationDegrees;
	Result.DebrisGroundMaximumAzimuthDeviationDegrees = DebrisGroundMaximumAzimuthDeviationDegrees;
	Result.DebrisGroundScatterFraction = DebrisGroundScatterFraction;
	Result.DebrisGroundElevationDegrees = FVector2f(DebrisGroundElevationDegrees);
	Result.DebrisGroundSpeedMultiplier = FVector2f(DebrisGroundSpeedMultiplier);
	Result.GravityZ = GravityZ;
	Result.GroundPlaneZ = GroundPlaneZ;
	Result.DebrisSettlingSeconds = DebrisSettlingSeconds;
	Result.NetworkLeadSeconds = NetworkLeadSeconds;
	Result.EncodingEstimateSeconds = EncodingEstimateSeconds;
	Result.QueueSafetySeconds = QueueSafetySeconds;
	Result.MaximumWorkPages = MaximumWorkPages;
	Result.MaximumQueryTilesPerPump = MaximumQueryTilesPerPump;
	Result.MaximumDestructionTargetsPerPump = MaximumDestructionTargetsPerPump;
	Result.MaximumDestructionTargetsPerSnapshotBatch = MaximumDestructionTargetsPerSnapshotBatch;
	Result.LocalServerGameplayBudgetMilliseconds = LocalServerGameplayBudgetMilliseconds;
	Result.DedicatedServerGameplayBudgetMilliseconds = DedicatedServerGameplayBudgetMilliseconds;
	Result.LocalServerWorkerConcurrency = LocalServerWorkerConcurrency;
	Result.DedicatedServerWorkerConcurrency = DedicatedServerWorkerConcurrency;
	Result.MaximumSettlementBatchSize = MaximumSettlementBatchSize;
	Result.SettlementPriorityCellSize = SettlementPriorityCellSize;
	Result.SettlementPriorityRadius = SettlementPriorityRadius;
	Result.MinimumGlobalSettlementCountPerBatch = MinimumGlobalSettlementCountPerBatch;
	Result.ClientGameThreadBudgetMilliseconds = ClientGameThreadBudgetMilliseconds;
	return Result;
}
