#include "ElementPresentationTypes.h"

namespace
{
	float CalculateRecenterAngle(
		const float CoverageAngle,
		const float CurrentFOV,
		const float Safety,
		const float Minimum)
	{
		return FMath::Max(Minimum, (CoverageAngle - CurrentFOV) * 0.5f - Safety);
	}
}

bool FElementPresentationConfig::IsValid() const
{
	return FMath::IsFinite(ShardSize) && ShardSize > UE_DOUBLE_SMALL_NUMBER
		&& FMath::IsFinite(CoverageRadius) && CoverageRadius >= 0.0
		&& FMath::IsFinite(HorizontalCoverageAngleDegrees)
		&& HorizontalCoverageAngleDegrees > 0.0f && HorizontalCoverageAngleDegrees <= 360.0f
		&& FMath::IsFinite(VerticalCoverageAngleDegrees)
		&& VerticalCoverageAngleDegrees > 0.0f && VerticalCoverageAngleDegrees <= 360.0f
		&& FMath::IsFinite(SubjectRecenterDistance) && SubjectRecenterDistance >= 0.0
		&& FMath::IsFinite(ViewRecenterDistance) && ViewRecenterDistance >= 0.0
		&& FMath::IsFinite(FOVSafetyAngleDegrees) && FOVSafetyAngleDegrees >= 0.0f
		&& FMath::IsFinite(MinimumRecenterAngleDegrees) && MinimumRecenterAngleDegrees >= 0.0f
		&& FMath::IsFinite(FieldOfViewChangeThresholdDegrees) && FieldOfViewChangeThresholdDegrees >= 0.0f
		&& FMath::IsFinite(AspectRatioChangeThreshold) && AspectRatioChangeThreshold >= 0.0f
		&& ViewportDimensionChangeThreshold >= 0
		&& FMath::IsFinite(GraceSeconds) && GraceSeconds >= 0.0
		&& MaxApplyCommandsPerTick > 0
		&& FMath::IsFinite(MaxApplyMilliseconds) && MaxApplyMilliseconds > 0.0
		&& InstancesPerPage > 0
		&& MaxSparePagesPerBackend >= 0
		&& MaxCoverageShardsPerSource > 0;
}

float FElementPresentationConfig::CalculateHorizontalRecenterAngleDegrees(
	const float CurrentHorizontalFOVDegrees) const
{
	return CalculateRecenterAngle(
		HorizontalCoverageAngleDegrees,
		CurrentHorizontalFOVDegrees,
		FOVSafetyAngleDegrees,
		MinimumRecenterAngleDegrees);
}

float FElementPresentationConfig::CalculateVerticalRecenterAngleDegrees(
	const float CurrentVerticalFOVDegrees) const
{
	return CalculateRecenterAngle(
		VerticalCoverageAngleDegrees,
		CurrentVerticalFOVDegrees,
		FOVSafetyAngleDegrees,
		MinimumRecenterAngleDegrees);
}
