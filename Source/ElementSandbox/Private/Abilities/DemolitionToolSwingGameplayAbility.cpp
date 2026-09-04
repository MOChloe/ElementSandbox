#include "Abilities/DemolitionToolSwingGameplayAbility.h"

#include "Animation/AnimSequenceBase.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Equipment/EquipmentComponent.h"
#include "Item/ItemInstance.h"
#include "Items/DemolitionToolItemFeature.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

UDemolitionToolSwingGameplayAbility::UDemolitionToolSwingGameplayAbility()
{
	static ConstructorHelpers::FObjectFinder<UAnimSequenceBase> DemolitionSwingAnimation(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"));
	SwingAnimation = DemolitionSwingAnimation.Object;

	FGameplayTagContainer DemolitionAbilityTags;
	DemolitionAbilityTags.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem_DemolitionTool);
	SetAssetTags(DemolitionAbilityTags);
	ActivationOwnedTags.AddTag(ElementSandboxGameplayTags::Ability_State_UsingEquippedItem);
	BlockAbilitiesWithTag.AddTag(ElementSandboxGameplayTags::Ability_Type_EquippedItem);
}

bool UDemolitionToolSwingGameplayAbility::CanActivateAbility(
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
	const UEquipmentComponent* Equipment = ActorInfo->AvatarActor->FindComponentByClass<UEquipmentComponent>();
	return IsValid(SourceItem)
		&& SourceItem->FindFeature<UDemolitionToolItemFeature>()
		&& Equipment
		&& Equipment->GetCurrentEquippedItem() == SourceItem;
}

void UDemolitionToolSwingGameplayAbility::ActivateAbility(
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
	if (!Character || !Character->PlayPredictedUpperBodyAnimation(SwingAnimation, 0.1f, 0.15f, PlayRate))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	bStartedAnimation = true;
	const float Duration = FMath::Max(0.01f, SwingAnimation->GetPlayLength() / PlayRate);
	Character->GetWorldTimerManager().SetTimer(
		EndTimerHandle,
		this,
		&UDemolitionToolSwingGameplayAbility::FinishSwing,
		Duration,
		false);
}

void UDemolitionToolSwingGameplayAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const bool bReplicateEndAbility,
	const bool bWasCancelled)
{
	if (ActorInfo)
	{
		if (AElementSandboxCharacter* Character = Cast<AElementSandboxCharacter>(ActorInfo->AvatarActor.Get()))
		{
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

void UDemolitionToolSwingGameplayAbility::FinishSwing()
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
