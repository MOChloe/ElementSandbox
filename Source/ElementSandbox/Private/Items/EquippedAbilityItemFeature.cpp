#include "Items/EquippedAbilityItemFeature.h"

#include "Net/UnrealNetwork.h"

void UEquippedAbilityItemFeature::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UEquippedAbilityItemFeature, AbilitySet);
}
