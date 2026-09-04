#pragma once

#include "CoreMinimal.h"
#include "Abilities/ToolSwingGameplayAbilityBase.h"

#include "AxeSwingGameplayAbility.generated.h"

/** 本地预测斧头动画；命中时由服务器用权威空间索引重新选择最近目标。 */
UCLASS()
class UAxeSwingGameplayAbility final : public UToolSwingGameplayAbilityBase
{
	GENERATED_BODY()

protected:
	virtual void OnAuthorityImpact(const FGameplayAbilityActorInfo* ActorInfo) override;
};

