#pragma once

#include "Abilities/ElementGameplayAbility.h"
#include "CoreMinimal.h"

#include "DemolitionToolSwingGameplayAbility.generated.h"

class UAnimSequenceBase;

/**
 * 拆除锤自己的本地预测挥击 Ability。只播放工具动作；拆除目标与结果仍由 Focus/Authority 事务决定。
 */
UCLASS()
class UDemolitionToolSwingGameplayAbility final : public UElementGameplayAbility
{
	GENERATED_BODY()

public:
	UDemolitionToolSwingGameplayAbility();

	virtual bool CanActivateAbility(
		FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(
		FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

private:
	/** 当前直接引用与挥木棍相同的动画资产；两套 Ability 不共享行为实现。 */
	UPROPERTY()
	TObjectPtr<UAnimSequenceBase> SwingAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Animation", meta = (ClampMin = "0.01"))
	float PlayRate = 1.0f;

	FTimerHandle EndTimerHandle;
	bool bStartedAnimation = false;

	void FinishSwing();
};
