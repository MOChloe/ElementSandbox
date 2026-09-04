#pragma once

#include "CoreMinimal.h"
#include "Equipment/EquippedItemActor.h"

#include "DemolitionToolEquippedItemActor.generated.h"

class UStaticMeshComponent;

/** 玩家手中的无碰撞拆除锤表现；不保存拆除目标或 Gameplay 状态。 */
UCLASS()
class ADemolitionToolEquippedItemActor final : public AEquippedItemActor
{
	GENERATED_BODY()

public:
	ADemolitionToolEquippedItemActor();

private:
	UPROPERTY(VisibleAnywhere, Category="Item")
	TObjectPtr<UStaticMeshComponent> HammerHead;
};
