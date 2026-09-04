#pragma once

#include "Math/UnrealMathUtility.h"

namespace UE::ElementSandbox::Meteor::Profile
{
	inline constexpr double MinStrikeDelaySeconds = 0.25;
	inline constexpr double MaxStrikeDelaySeconds = 600.0;
	inline constexpr double MinAuthorityImpactTimeSeconds = 1.0;
	inline constexpr double MaxAuthorityImpactTimeSeconds = 3600.0;

	/**
	 * Profile 延迟从 Client 主进程原样转交 Authority Server Child。上限只防止误输入；
	 * 不能压回短延迟，否则慢 RHI 尚未完成 Residency 时就会得到不同的压测场景。
	 */
	inline double SanitizeStrikeDelaySeconds(const double DelaySeconds)
	{
		return FMath::IsFinite(DelaySeconds)
			? FMath::Clamp(DelaySeconds, MinStrikeDelaySeconds, MaxStrikeDelaySeconds)
			: MinStrikeDelaySeconds;
	}

	/** 固定 Authority WorldTime 让不同 RHI 在同一个 Gameplay 时刻撞击。 */
	inline double SanitizeAuthorityImpactTimeSeconds(const double ImpactTimeSeconds)
	{
		return FMath::IsFinite(ImpactTimeSeconds)
			? FMath::Clamp(
				ImpactTimeSeconds,
				MinAuthorityImpactTimeSeconds,
				MaxAuthorityImpactTimeSeconds)
			: MinAuthorityImpactTimeSeconds;
	}
}
