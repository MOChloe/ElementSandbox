#pragma once

#include "CoreMinimal.h"
#include "Abilities/ToolSwingGameplayAbilityBase.h"
#include "Entity/WorldEntityId.h"
#include "StickSwingGameplayAbility.generated.h"

/**
 * 木棍保留自己的 Fire Interaction 语义；动画与时间控制由中性挥动基类提供。
 */
UCLASS()
class UStickSwingGameplayAbility final : public UToolSwingGameplayAbilityBase
{
	GENERATED_BODY()

protected:
	virtual void OnSwingStarted(const FGameplayAbilityActorInfo* ActorInfo) override;
	virtual void OnSwingEnded(const FGameplayAbilityActorInfo* ActorInfo) override;

private:
	FWorldEntityId FireInteractionEntity;
	void SetAuthorityFireInteraction(
		const FGameplayAbilityActorInfo* ActorInfo,
		bool bActive);
};
