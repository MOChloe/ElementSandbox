#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ElementGameplayAbility.generated.h"

/** 玩家主动行为的公共基类；默认使用按 Actor 实例化和本地预测。 */
UCLASS(Abstract, Blueprintable)
class ELEMENTSANDBOXABILITIES_API UElementGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UElementGameplayAbility();
};
