#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"

#include "BuildDamageFragment.generated.h"

/** 首次受击时才创建的会话态伤害；不进入 Building Payload。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildDamageFragment final : public FBuildFragment
{
	GENERATED_BODY()

	float AccumulatedDamage = 0.0f;
	uint32 DestructionRevision = 1;
};

