#pragma once

#include "CoreMinimal.h"
#include "Item/ItemDefinition.h"

#include "ReclaimedBuildingItemDefinition.generated.h"

/**
 * 程序化城镇构件的不可堆叠背包载体。
 * 具体 Building Definition、Rotation 与 Scale 位于每个 ItemInstance 独占的 BuildingItemFeature。
 */
UCLASS(NotBlueprintable)
class UReclaimedBuildingItemDefinition final : public UItemDefinition
{
	GENERATED_BODY()

public:
	UReclaimedBuildingItemDefinition();
};
