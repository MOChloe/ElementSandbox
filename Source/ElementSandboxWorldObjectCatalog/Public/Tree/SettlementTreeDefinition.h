#pragma once

#include "Definition/WorldObjectDefinition.h"

#include "SettlementTreeDefinition.generated.h"

/** Settlement.Tree 只定义 Gameplay 空间事实；视觉资源只在非 Dedicated 表现子系统创建。 */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API USettlementTreeDefinition final : public UWorldObjectDefinition
{
	GENERATED_BODY()

public:
	USettlementTreeDefinition();
};
