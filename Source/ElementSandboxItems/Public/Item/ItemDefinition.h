#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Item/InventoryItemDefinition.h"
#include "ItemDefinition.generated.h"

class UItemFeature;

/** 只描述一个道具由哪些 Feature 组成，不承载功能开关和运行时状态。 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXITEMS_API UItemDefinition
	: public UPrimaryDataAsset
	, public IInventoryItemDefinition
{
	GENERATED_BODY()

public:
	/** 创建 ItemInstance 时，每个模板都会被 DuplicateObject 为该实例独占的 Feature。 */
	UPROPERTY(EditDefaultsOnly, Instanced, Category="Item")
	TArray<TObjectPtr<UItemFeature>> FeatureTemplates;

	virtual const TArray<TObjectPtr<UItemFeature>>& GetItemFeatureTemplates() const override
	{
		return FeatureTemplates;
	}

	template <typename FeatureType>
	const FeatureType* FindFeatureTemplate() const
	{
		for (const UItemFeature* Feature : FeatureTemplates)
		{
			if (const FeatureType* TypedFeature = Cast<FeatureType>(Feature))
			{
				return TypedFeature;
			}
		}
		return nullptr;
	}
};
