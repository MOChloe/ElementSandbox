#pragma once

#include "CoreMinimal.h"
#include "Item/ItemFeature.h"

#include "BuildingItemFeature.generated.h"

/** 道具实例指向可摆放 Building Definition 的跨域桥接数据。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UBuildingItemFeature final : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	FName GetBuildingDefinitionId() const { return BuildingDefinitionId; }
	const FTransform& GetPlacementShapeTransform() const { return PlacementShapeTransform; }

	/**
	 * 仅服务器在不可堆叠的回收构件加入背包、事务提交前调用。
	 * Location 必须为零；Rotation/Scale 描述再次摆放时需要保留的实例形态。
	 */
	bool ConfigureReclaimedBuilding(
		FName InBuildingDefinitionId,
		const FTransform& InPlacementShapeTransform);

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_BuildingData, Category="Building")
	FName BuildingDefinitionId = NAME_None;

	/** 默认建造物品为 Identity；回收的城镇构件按 ItemInstance 独立保存原旋转与缩放。 */
	UPROPERTY(EditDefaultsOnly, ReplicatedUsing=OnRep_BuildingData, Category="Building")
	FTransform PlacementShapeTransform = FTransform::Identity;

private:
	UFUNCTION()
	void OnRep_BuildingData();
};
