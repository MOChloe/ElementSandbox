#pragma once

#include "CoreMinimal.h"
#include "Item/ItemDefinition.h"

#include "AxeItemDefinition.generated.h"

/** 纯 C++、稳定命名的斧头 Definition；当前只提供装备表现和挥击动作。 */
UCLASS()
class UAxeItemDefinition final : public UItemDefinition
{
	GENERATED_BODY()

public:
	UAxeItemDefinition();
};
