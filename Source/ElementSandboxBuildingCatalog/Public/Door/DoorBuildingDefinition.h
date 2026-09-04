#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"
#include "Item/InventoryItemDefinition.h"

#include "DoorBuildingDefinition.generated.h"

class FBuildEntityRegistry;
class UItemFeature;

/**
 * Door 的共享 C++ 配置对象；当前作为本应由 DataAsset 配置的伪 Blueprint。
 * 它既能创建带初始关闭状态的 Door Entity，也能作为背包 ItemInstance 的定义来源。
 */
UCLASS(BlueprintType)
class ELEMENTSANDBOXBUILDINGCATALOG_API UDoorBuildingDefinition final
	: public UBuildingDefinition
	, public IInventoryItemDefinition
{
	GENERATED_BODY()

public:
	UDoorBuildingDefinition();
	/** 将同一套七部件门配置成聚落 Descriptor 创建的确定性伴生门。 */
	bool InitializeAsSettlementCompanion();
	virtual bool TryCalculateWorldBounds(
		const FTransform& WorldTransform,
		TConstArrayView<FTransform> PartLocalTransforms,
		FBox& OutWorldBounds) const override;
	virtual bool DoPartTransformChangesAffectSpatialBounds(
		TConstArrayView<int32> PartIds) const override;

	/** Door 进入背包时复制给 ItemInstance 的 Feature 模板。 */
	UPROPERTY(EditDefaultsOnly, Instanced, Category="Door|Inventory")
	TArray<TObjectPtr<UItemFeature>> ItemFeatureTemplates;

	virtual const TArray<TObjectPtr<UItemFeature>>& GetItemFeatureTemplates() const override
	{
		return ItemFeatureTemplates;
	}

protected:
	virtual bool ConfigureEntity(
		FBuildEntityRegistry& Registry,
		FBuildEntityHandle Entity) const override;
};
