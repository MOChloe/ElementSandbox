#pragma once

#include "CoreMinimal.h"
#include "Equipment/EquippedItemActor.h"

#include "FireballEquippedItemActor.generated.h"

/** 玩家手中的无碰撞火焰球占位表现。 */
UCLASS()
class AFireballEquippedItemActor final : public AEquippedItemActor
{
	GENERATED_BODY()

public:
	AFireballEquippedItemActor();
};
