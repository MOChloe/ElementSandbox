#include "Abilities/EquipmentAbilityBridgeComponent.h"

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "ElementSandbox.h"
#include "Equipment/EquipmentComponent.h"
#include "Item/ItemFeature.h"
#include "Item/ItemInstance.h"
#include "Items/EquippedAbilityItemFeature.h"

UEquipmentAbilityBridgeComponent::UEquipmentAbilityBridgeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UEquipmentAbilityBridgeComponent::InitializeAbilitySystem(
	UElementAbilitySystemComponent* NewAbilitySystem)
{
	EnsureEquipmentBinding();
	RevokeGrantedAbilities();
	AbilitySystem = NewAbilitySystem;

	if (AbilitySystem && GetOwner() && GetOwner()->HasAuthority() && BoundEquipment)
	{
		GrantAbilitiesFromItem(BoundEquipment->GetCurrentEquippedItem());
	}
}

void UEquipmentAbilityBridgeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RevokeGrantedAbilities();
	if (BoundEquipment)
	{
		BoundEquipment->OnEquippedItemChanged().RemoveAll(this);
	}
	BoundEquipment = nullptr;
	AbilitySystem = nullptr;

	Super::EndPlay(EndPlayReason);
}

void UEquipmentAbilityBridgeComponent::EnsureEquipmentBinding()
{
	UEquipmentComponent* CurrentEquipment = GetOwner()
		? GetOwner()->FindComponentByClass<UEquipmentComponent>()
		: nullptr;
	if (CurrentEquipment == BoundEquipment)
	{
		return;
	}

	if (BoundEquipment)
	{
		BoundEquipment->OnEquippedItemChanged().RemoveAll(this);
	}
	BoundEquipment = CurrentEquipment;
	if (BoundEquipment)
	{
		BoundEquipment->OnEquippedItemChanged().AddUObject(
			this,
			&UEquipmentAbilityBridgeComponent::HandleEquippedItemChanged);
	}
}

void UEquipmentAbilityBridgeComponent::HandleEquippedItemChanged(
	UItemInstance* PreviousItem,
	UItemInstance* NewItem)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	RevokeGrantedAbilities();
	GrantAbilitiesFromItem(NewItem);
}

void UEquipmentAbilityBridgeComponent::GrantAbilitiesFromItem(UItemInstance* ItemInstance)
{
	if (!AbilitySystem || !IsValid(ItemInstance))
	{
		return;
	}

	const UEquippedAbilityItemFeature* AbilityFeature = nullptr;
	for (const UItemFeature* Feature : ItemInstance->GetFeatures())
	{
		const UEquippedAbilityItemFeature* Candidate = Cast<UEquippedAbilityItemFeature>(Feature);
		if (!Candidate)
		{
			continue;
		}

		if (AbilityFeature)
		{
			UE_LOG(
				LogElementSandbox,
				Error,
				TEXT("道具 %s 配置了多个 EquippedAbilityItemFeature，拒绝授予 Ability。"),
				*GetNameSafe(ItemInstance));
			return;
		}
		AbilityFeature = Candidate;
	}

	if (!AbilityFeature || AbilityFeature->GetAbilitySet().Abilities.IsEmpty())
	{
		return;
	}

	GrantedAbilityHandles = AbilitySystem->GrantAbilitySet(
		AbilityFeature->GetAbilitySet(),
		ItemInstance);
	if (GrantedAbilityHandles.Num() == AbilityFeature->GetAbilitySet().Abilities.Num())
	{
		GrantedSourceItem = ItemInstance;
	}
	else
	{
		AbilitySystem->RevokeAbilitySet(GrantedAbilityHandles);
	}
}

void UEquipmentAbilityBridgeComponent::RevokeGrantedAbilities()
{
	if (AbilitySystem)
	{
		AbilitySystem->RevokeAbilitySet(GrantedAbilityHandles);
	}
	else
	{
		GrantedAbilityHandles.Reset();
	}
	GrantedSourceItem = nullptr;
}
