#include "Game/ElementSandboxPlayerState.h"

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Attributes/ElementCharacterAttributeSet.h"
#include "Inventory/InventoryComponent.h"

AElementSandboxPlayerState::AElementSandboxPlayerState()
{
	bReplicateUsingRegisteredSubObjectList = true;
	SetNetUpdateFrequency(100.0f);
	SetMinNetUpdateFrequency(33.0f);

	AbilitySystemComponent = CreateDefaultSubobject<UElementAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CharacterAttributes = CreateDefaultSubobject<UElementCharacterAttributeSet>(TEXT("CharacterAttributes"));
	// AttributeSet 的 Outer 是 PlayerState，不是 ASC；必须显式注册后 GameplayEffect 才能找到并修改它。
	AbilitySystemComponent->AddAttributeSetSubobject(CharacterAttributes.Get());
	InventoryComponent = CreateDefaultSubobject<UInventoryComponent>(TEXT("InventoryComponent"));
}

UAbilitySystemComponent* AElementSandboxPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}
