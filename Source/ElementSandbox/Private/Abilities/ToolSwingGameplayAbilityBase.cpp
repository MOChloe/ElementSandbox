#include "Abilities/ToolSwingGameplayAbilityBase.h"

#include "Animation/AnimSequenceBase.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Equipment/EquipmentComponent.h"
#include "Item/ItemInstance.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

UToolSwingGameplayAbilityBase::UToolSwingGameplayAbilityBase()
{
	static ConstructorHelpers::FObjectFinder<UAnimSequenceBase> DefaultSwingAnimation(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"));
	SwingAnimation = DefaultSwingAnimation.Object;

	FGameplayTagContainer SwingAbilityTags;
	SwingAbilityTags.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem_Swing);
	SetAssetTags(SwingAbilityTags);
	ActivationOwnedTags.AddTag(ElementSandboxGameplayTags::Ability_State_UsingEquippedItem);
	BlockAbilitiesWithTag.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem);
}

bool UToolSwingGameplayAbilityBase::CanActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)
		|| !ActorInfo
		|| !Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get())
		|| !IsValid(SwingAnimation))
	{
		return false;
	}
	if (!ActorInfo->IsNetAuthority())
	{
		return true;
	}

	const UItemInstance* SourceItem = Cast<UItemInstance>(GetSourceObject(Handle, ActorInfo));
	const UEquipmentComponent* Equipment =
		ActorInfo->AvatarActor->FindComponentByClass<UEquipmentComponent>();
	return IsValid(SourceItem) && Equipment && Equipment->GetCurrentEquippedItem() == SourceItem;
}

void UToolSwingGameplayAbilityBase::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	AElementSandboxCharacter* Character = ActorInfo
		? Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get())
		: nullptr;
	if (!Character
		|| !Character->PlayPredictedUpperBodyAnimation(
			SwingAnimation,
			0.1f,
			0.15f,
			PlayRate))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	bStartedAnimation = true;
	OnSwingStarted(ActorInfo);
	const float Duration = FMath::Max(0.01f, SwingAnimation->GetPlayLength() / PlayRate);
	Character->GetWorldTimerManager().SetTimer(
		ImpactTimerHandle,
		this,
		&UToolSwingGameplayAbilityBase::HandleImpact,
		FMath::Clamp(Duration * ImpactTimeFraction, 0.0f, Duration),
		false);
	Character->GetWorldTimerManager().SetTimer(
		EndTimerHandle,
		this,
		&UToolSwingGameplayAbilityBase::FinishSwing,
		Duration,
		false);
}

void UToolSwingGameplayAbilityBase::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const bool bReplicateEndAbility,
	const bool bWasCancelled)
{
	OnSwingEnded(ActorInfo);
	if (ActorInfo)
	{
		if (AElementSandboxCharacter* Character =
			Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()))
		{
			Character->GetWorldTimerManager().ClearTimer(ImpactTimerHandle);
			Character->GetWorldTimerManager().ClearTimer(EndTimerHandle);
			if (bWasCancelled && bStartedAnimation)
			{
				Character->StopPredictedUpperBodyAnimation();
			}
		}
	}
	bStartedAnimation = false;
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UToolSwingGameplayAbilityBase::HandleImpact()
{
	const FGameplayAbilityActorInfo* ActorInfo = GetCurrentActorInfo();
	if (IsActive() && ActorInfo && ActorInfo->IsNetAuthority())
	{
		OnAuthorityImpact(ActorInfo);
	}
}

void UToolSwingGameplayAbilityBase::FinishSwing()
{
	if (IsActive())
	{
		EndAbility(
			GetCurrentAbilitySpecHandle(),
			GetCurrentActorInfo(),
			GetCurrentActivationInfo(),
			true,
			false);
	}
}

