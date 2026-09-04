#pragma once

#include "CoreMinimal.h"
#include "Item/ItemDefinition.h"

#include "WoodBlockItemDefinition.generated.h"

/** 每个 WorldObject 木块拾取后得到一个可堆叠木块道具。 */
UCLASS(NotBlueprintable)
class UWoodBlockItemDefinition final : public UItemDefinition
{
	GENERATED_BODY()

public:
	UWoodBlockItemDefinition();
};

