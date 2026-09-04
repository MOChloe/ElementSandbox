#pragma once

#include "CoreMinimal.h"
#include "Item/InventoryItemDefinition.h"

#include "ItemDefinitionTestTypes.generated.h"

class UItemFeature;

/** 测试背包定义契约不要求继承 UItemDefinition。 */
UCLASS()
class UTestInventoryItemDefinition final
	: public UObject
	, public IInventoryItemDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(Instanced)
	TArray<TObjectPtr<UItemFeature>> FeatureTemplates;

	virtual const TArray<TObjectPtr<UItemFeature>>& GetItemFeatureTemplates() const override
	{
		return FeatureTemplates;
	}
};
