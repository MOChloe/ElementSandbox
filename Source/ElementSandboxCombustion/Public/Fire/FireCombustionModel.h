#pragma once

#include "CoreMinimal.h"
#include "Fire/FireCombustionTypes.h"

/** 无 UObject、无世界访问、可在 Worker 执行的 Gameplay 火焰解析器。 */
class ELEMENTSANDBOXCOMBUSTION_API FFireCombustionModel final
{
public:
	static bool ValidateProfile(const FFireCombustionProfile& Profile, FString* OutError = nullptr);
	static bool ValidateState(
		const FFireCombustionState& State,
		const FFireCombustionProfile& Profile,
		FString* OutError = nullptr);
	static FFireIntervalResult AdvanceInterval(
		const FFireCombustionState& InitialState,
		const FFireCombustionProfile& Profile,
		const FFireIntervalInput& Input);

	static double ComputeDistanceAttenuation(double SurfaceDistanceCentimeters, double RangeCentimeters);
};
