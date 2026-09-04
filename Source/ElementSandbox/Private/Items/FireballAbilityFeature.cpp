#include "Items/FireballAbilityFeature.h"

#include "Abilities/FireballGameplayAbility.h"
#include "Tags/ElementGameplayTags.h"

UFireballAbilityFeature::UFireballAbilityFeature()
{
	FElementAbilityGrant& FireballGrant = AbilitySet.Abilities.AddDefaulted_GetRef();
	FireballGrant.Ability = UFireballGameplayAbility::StaticClass();
	FireballGrant.InputTag = ElementSandboxGameplayTags::Input_Use_Primary;
	FireballGrant.AbilityLevel = 1;
}
