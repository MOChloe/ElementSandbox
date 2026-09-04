#include "Items/MeteorStrikeAbilityFeature.h"

#include "Abilities/MeteorStrikeGameplayAbility.h"
#include "Tags/ElementGameplayTags.h"

UMeteorStrikeAbilityFeature::UMeteorStrikeAbilityFeature()
{
	FElementAbilityGrant& Grant = AbilitySet.Abilities.AddDefaulted_GetRef();
	Grant.Ability = UMeteorStrikeGameplayAbility::StaticClass();
	Grant.InputTag = ElementSandboxGameplayTags::Input_Use_Primary;
	Grant.AbilityLevel = 1;
}
