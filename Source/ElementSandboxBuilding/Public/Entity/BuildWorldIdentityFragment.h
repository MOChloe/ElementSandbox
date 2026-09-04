#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"
#include "Entity/WorldEntityId.h"

#include "BuildWorldIdentityFragment.generated.h"

/** Building 的永久世界身份与权威状态版本；本地 Handle 不得跨 World 传递。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildWorldIdentityFragment final
	: public FBuildFragment
{
	GENERATED_BODY()

	FWorldEntityId WorldEntityId;
	uint32 StateRevision = 1;
};
