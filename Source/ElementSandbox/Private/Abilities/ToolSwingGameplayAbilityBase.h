#pragma once

#include "CoreMinimal.h"
#include "Abilities/ElementGameplayAbility.h"

#include "ToolSwingGameplayAbilityBase.generated.h"

class UAnimSequenceBase;

/** 木棍与斧头共享的本地预测挥动时序；具体 Gameplay 后果由派生 Ability 独占。 */
UCLASS(Abstract)
class UToolSwingGameplayAbilityBase : public UElementGameplayAbility
{
	GENERATED_BODY()

public:
	UToolSwingGameplayAbilityBase();

	virtual bool CanActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

protected:
	virtual void OnSwingStarted(const FGameplayAbilityActorInfo* ActorInfo) {}
	virtual void OnAuthorityImpact(const FGameplayAbilityActorInfo* ActorInfo) {}
	virtual void OnSwingEnded(const FGameplayAbilityActorInfo* ActorInfo) {}

private:
	UPROPERTY()
	TObjectPtr<UAnimSequenceBase> SwingAnimation;

	UPROPERTY(EditDefaultsOnly, Category="Animation", meta=(ClampMin="0.01"))
	float PlayRate = 1.0f;

	UPROPERTY(EditDefaultsOnly, Category="Animation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ImpactTimeFraction = 0.35f;

	FTimerHandle ImpactTimerHandle;
	FTimerHandle EndTimerHandle;
	bool bStartedAnimation = false;

	void HandleImpact();
	void FinishSwing();
};

