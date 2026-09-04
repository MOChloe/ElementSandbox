#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectFragment.h"

#include "WorldObjectDamageFragment.generated.h"

/** 首次受击时才创建的会话态伤害；RuntimeEvict/重新注入后自然恢复满耐久。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectDamageFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	float AccumulatedDamage = 0.0f;
	uint32 DestructionRevision = 1;
};

