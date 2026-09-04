#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "WorldObjectEquipmentBridgeComponent.generated.h"

class UInventoryComponent;
class UItemInstance;

/** 把装备事件装配为 WorldObject 生命周期；不属于 Items 或 WorldObjects 底层模块。 */
UCLASS(NotBlueprintable, ClassGroup=(WorldObject))
class UWorldObjectEquipmentBridgeComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectEquipmentBridgeComponent();

	/** 服务器事务：把当前手持木棍原地转换为同一 WorldObject 的 Chaos 投掷物。 */
	bool ThrowSelectedStick(
		UInventoryComponent& Inventory,
		const FVector& ForwardDirection,
		double ForwardSpeed,
		double UpwardSpeed);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void HandleEquippedItemChanged(UItemInstance* PreviousItem, UItemInstance* NewItem);
	void CreateAttachedProjection(UItemInstance& ItemInstance);
};
