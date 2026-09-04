#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"

#include "WorldObjectFocusTarget.generated.h"

/** Focus 层只携带网络身份；绝不把本地 Entity Handle 发给服务器。 */
USTRUCT()
struct FWorldObjectFocusTarget final
{
	GENERATED_BODY()

	FWorldEntityId WorldEntityId;

	bool IsValid() const { return WorldEntityId.IsSet(); }
};
