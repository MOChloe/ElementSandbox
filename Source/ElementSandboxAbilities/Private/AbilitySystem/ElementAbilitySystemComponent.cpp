#include "AbilitySystem/ElementAbilitySystemComponent.h"

#include "Abilities/GameplayAbility.h"
#include "ElementSandboxAbilities.h"

UElementAbilitySystemComponent::UElementAbilitySystemComponent()
{
	SetIsReplicatedByDefault(true);
}

void UElementAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	ABILITYLIST_SCOPE_LOCK();
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.Ability
			|| !AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			continue;
		}

		AbilitySpecInputPressed(AbilitySpec);
		if (AbilitySpec.IsActive())
		{
			ForwardReplicatedInputEvent(EAbilityGenericReplicatedEvent::InputPressed, AbilitySpec);
		}
		else
		{
			TryActivateAbility(AbilitySpec.Handle);
		}
	}
}

void UElementAbilitySystemComponent::AbilityInputTagReleased(const FGameplayTag InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	ABILITYLIST_SCOPE_LOCK();
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.Ability
			|| !AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			continue;
		}

		AbilitySpecInputReleased(AbilitySpec);
		if (AbilitySpec.IsActive())
		{
			ForwardReplicatedInputEvent(EAbilityGenericReplicatedEvent::InputReleased, AbilitySpec);
		}
	}
}

TArray<FGameplayAbilitySpecHandle> UElementAbilitySystemComponent::GrantAbilitySet(
	const FElementAbilitySet& AbilitySet,
	UObject* SourceObject)
{
	TArray<FGameplayAbilitySpecHandle> GrantedHandles;
	if (!IsOwnerActorAuthoritative() || !IsValid(SourceObject))
	{
		return GrantedHandles;
	}

	for (const FElementAbilityGrant& Grant : AbilitySet.Abilities)
	{
		if (!Grant.Ability || Grant.AbilityLevel < 1)
		{
			UE_LOG(
				LogElementSandboxAbilities,
				Error,
				TEXT("来源 %s 的 AbilitySet 包含无效 Ability 或等级。整组拒绝授予。"),
				*GetNameSafe(SourceObject));
			return {};
		}
	}

	GrantedHandles.Reserve(AbilitySet.Abilities.Num());
	for (const FElementAbilityGrant& Grant : AbilitySet.Abilities)
	{
		FGameplayAbilitySpec AbilitySpec(Grant.Ability, Grant.AbilityLevel, INDEX_NONE, SourceObject);
		if (Grant.InputTag.IsValid())
		{
			AbilitySpec.GetDynamicSpecSourceTags().AddTag(Grant.InputTag);
		}
		GrantedHandles.Add(GiveAbility(AbilitySpec));
	}
	return GrantedHandles;
}

void UElementAbilitySystemComponent::RevokeAbilitySet(TArray<FGameplayAbilitySpecHandle>& GrantedHandles)
{
	if (!IsOwnerActorAuthoritative())
	{
		return;
	}

	for (const FGameplayAbilitySpecHandle Handle : GrantedHandles)
	{
		if (Handle.IsValid())
		{
			ClearAbility(Handle);
		}
	}
	GrantedHandles.Reset();
}

void UElementAbilitySystemComponent::ForwardReplicatedInputEvent(
	const EAbilityGenericReplicatedEvent::Type EventType,
	const FGameplayAbilitySpec& AbilitySpec)
{
	UGameplayAbility* AbilityInstance = AbilitySpec.GetPrimaryInstance();
	if (!AbilityInstance)
	{
		return;
	}

	InvokeReplicatedEvent(
		EventType,
		AbilitySpec.Handle,
		AbilityInstance->GetCurrentActivationInfo().GetActivationPredictionKey());
}
