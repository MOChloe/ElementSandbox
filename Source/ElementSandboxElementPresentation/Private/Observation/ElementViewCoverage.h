#pragma once

#include "CoreMinimal.h"
#include "ElementPresentationTypes.h"
#include "PresentationViewSource.h"

struct FElementPresentationViewState final
{
	FPresentationViewSource Latest;
	FPresentationViewSource Anchor;
	TSet<FElementVisualShardKey> Coverage;
	bool bHasAnchor = false;
};

namespace ElementViewCoverage
{
	bool CrossesInvalidationThreshold(
		const FPresentationViewSource& Anchor,
		const FPresentationViewSource& Latest,
		const FElementPresentationConfig& Config);

	/** 只枚举观察半径内的稀疏格坐标，成本不随 Visual Resident 总量增长。 */
	bool BuildCoverage(
		const FPresentationViewSource& View,
		const FElementPresentationConfig& Config,
		TSet<FElementVisualShardKey>& OutCoverage);
}
