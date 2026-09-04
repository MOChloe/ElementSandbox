#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "InventoryItemDefinition.generated.h"

class UItemFeature;

/**
 * 可以作为 UItemInstance 定义来源的最小契约。
 * 接口只提供共享的 Feature 模板；槽位、数量和复制状态仍属于运行时 ItemInstance。
 */
UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UInventoryItemDefinition : public UInterface
{
	GENERATED_BODY()
};

class ELEMENTSANDBOXITEMS_API IInventoryItemDefinition
{
	GENERATED_BODY()

public:
	/** 创建 ItemInstance 时会把每个模板复制为该实例独占的 Feature。 */
	virtual const TArray<TObjectPtr<UItemFeature>>& GetItemFeatureTemplates() const = 0;
};
