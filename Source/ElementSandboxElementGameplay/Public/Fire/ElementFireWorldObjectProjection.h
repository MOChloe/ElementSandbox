#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "ElementFireWorldObjectProjection.generated.h"

class ACharacter;

UINTERFACE(MinimalAPI)
class UElementFireWorldObjectProjection : public UInterface
{
	GENERATED_BODY()
};

/** 上层 Actor 实现的薄投影接口，避免 ElementGameplay 反向依赖项目装配模块。 */
class ELEMENTSANDBOXELEMENTGAMEPLAY_API IElementFireWorldObjectProjection
{
	GENERATED_BODY()

public:
	virtual void ApplyElementFireBurning(bool bBurning) = 0;
	virtual void QueryElementFireContext(
		bool& bOutEquipped,
		ACharacter*& OutHolderCharacter) const = 0;
};
