#pragma once

#include "CoreMinimal.h"
#include "Item/ItemDefinition.h"

#include "DemolitionToolItemDefinition.generated.h"

/** 纯 C++、稳定命名的拆除锤 Definition。 */
UCLASS()
class UDemolitionToolItemDefinition final : public UItemDefinition
{
	GENERATED_BODY()

public:
	UDemolitionToolItemDefinition();
};
