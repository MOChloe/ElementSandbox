#pragma once

#include "Abilities/ElementGameplayAbility.h"
#include "CoreMinimal.h"

#include "MeteorStrikeGameplayAbility.generated.h"

class UAnimSequenceBase;

/** 客户端只预测挥手表现；服务器从角色前方的 Resident Chunk 选择统一地面展示落点。 */
UCLASS()
class UMeteorStrikeGameplayAbility final : public UElementGameplayAbility
{
	GENERATED_BODY()

public:
	UMeteorStrikeGameplayAbility();
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

private:
	bool ExecuteAuthorityStrike(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo& ActorInfo);
	void FinishUse();

	UPROPERTY()
	TObjectPtr<UAnimSequenceBase> UseAnimation;

	FTimerHandle EndTimerHandle;
};
