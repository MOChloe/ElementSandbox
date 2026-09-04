#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"

#include "BuildRenderCustomDataFragment.generated.h"

/** Entity 级共享的 PerInstance Custom Data；各 Mesh Part 按声明数量读取前 N 项。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildRenderCustomDataFragment final
	: public FBuildFragment
{
	GENERATED_BODY()

	TArray<float, TInlineAllocator<4>> Values;
};
