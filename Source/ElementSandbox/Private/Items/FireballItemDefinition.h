#pragma once

#include "CoreMinimal.h"
#include "Item/ItemDefinition.h"

#include "FireballItemDefinition.generated.h"

/** 纯 C++、稳定命名的火焰球 Definition；CDO 可作为联网 ItemInstance 的定义引用。 */
UCLASS()
class UFireballItemDefinition final : public UItemDefinition
{
	GENERATED_BODY()

public:
	UFireballItemDefinition();
};
