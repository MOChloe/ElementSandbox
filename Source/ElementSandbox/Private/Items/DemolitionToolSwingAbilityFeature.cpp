#include "Items/DemolitionToolSwingAbilityFeature.h"

#include "Abilities/DemolitionToolSwingGameplayAbility.h"
#include "Tags/ElementGameplayTags.h"

UDemolitionToolSwingAbilityFeature::UDemolitionToolSwingAbilityFeature()
{
	FElementAbilityGrant& SwingGrant = AbilitySet.Abilities.AddDefaulted_GetRef();
	SwingGrant.Ability = UDemolitionToolSwingGameplayAbility::StaticClass();
	SwingGrant.InputTag = ElementSandboxGameplayTags::Input_Use_Primary;
	SwingGrant.AbilityLevel = 1;
}
