#include "Items/AxeSwingAbilityFeature.h"

#include "Abilities/AxeSwingGameplayAbility.h"
#include "Tags/ElementGameplayTags.h"

UAxeSwingAbilityFeature::UAxeSwingAbilityFeature()
{
	FElementAbilityGrant& SwingGrant = AbilitySet.Abilities.AddDefaulted_GetRef();
	SwingGrant.Ability = UAxeSwingGameplayAbility::StaticClass();
	SwingGrant.InputTag = ElementSandboxGameplayTags::Input_Use_Primary;
	SwingGrant.AbilityLevel = 1;
}
