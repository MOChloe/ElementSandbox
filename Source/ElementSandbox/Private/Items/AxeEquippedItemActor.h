#pragma once

#include "CoreMinimal.h"
#include "Equipment/EquippedItemActor.h"

#include "AxeEquippedItemActor.generated.h"

class UStaticMeshComponent;

/** 玩家手中的无碰撞斧头表现；当前不保存命中、采集或 Gameplay 状态。 */
UCLASS()
class AAxeEquippedItemActor final : public AEquippedItemActor
{
	GENERATED_BODY()

public:
	AAxeEquippedItemActor();

private:
	UPROPERTY(VisibleAnywhere, Category="Item")
	TObjectPtr<UStaticMeshComponent> AxeHead;

	UPROPERTY(VisibleAnywhere, Category="Item")
	TObjectPtr<UStaticMeshComponent> AxeBlade;
};
