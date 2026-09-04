#pragma once

#include "Abilities/ElementGameplayAbility.h"
#include "CoreMinimal.h"

#include "FireballGameplayAbility.generated.h"

class AElementSandboxCharacter;
class AFireballProjectile;
class AFireballEquippedItemActor;
class UAnimSequenceBase;
class UItemInstance;
struct FPredictionKey;

/** 当前装备火焰球的本地预测投掷 Ability；元素后果只在 Authority 落点创建。 */
UCLASS()
class UFireballGameplayAbility final : public UElementGameplayAbility
{
	GENERATED_BODY()

  public:
	UFireballGameplayAbility();

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
	bool SpawnAuthorityProjectile(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo& ActorInfo,
		const FPredictionKey& PredictionKey);
	AFireballProjectile* SpawnLocalPredictedProjectile(const FGameplayAbilityActorInfo& ActorInfo,
		const FPredictionKey& PredictionKey);
	void FinishThrow();

	UPROPERTY()
	TObjectPtr<UAnimSequenceBase> ThrowAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Animation", meta = (ClampMin = "0.01"))
	float PlayRate = 1.25f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability", meta = (ClampMin = "0.05"))
	float UseLockSeconds = 0.4f;

	FTimerHandle EndTimerHandle;
	TWeakObjectPtr<AFireballEquippedItemActor> HiddenEquippedProjection;
	bool bStartedAnimation = false;
};
