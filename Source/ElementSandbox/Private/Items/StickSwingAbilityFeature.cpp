#include "Items/StickSwingAbilityFeature.h"

#include "Abilities/StickSwingGameplayAbility.h"
#include "Tags/ElementGameplayTags.h"

UStickSwingAbilityFeature::UStickSwingAbilityFeature()
{
	FElementAbilityGrant& SwingGrant = AbilitySet.Abilities.AddDefaulted_GetRef();
	SwingGrant.Ability = UStickSwingGameplayAbility::StaticClass();
	SwingGrant.InputTag = ElementSandboxGameplayTags::Input_Use_Primary;
	SwingGrant.AbilityLevel = 1;
}
