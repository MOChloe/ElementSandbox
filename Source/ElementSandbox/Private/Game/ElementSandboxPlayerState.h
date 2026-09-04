#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/PlayerState.h"
#include "ElementSandboxPlayerState.generated.h"

class UElementAbilitySystemComponent;
class UElementCharacterAttributeSet;
class UInventoryComponent;

/** PlayerState 持有跨 Pawn 生命周期的背包、ASC 与角色属性。 */
UCLASS()
class AElementSandboxPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AElementSandboxPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UElementAbilitySystemComponent* GetElementAbilitySystemComponent() const { return AbilitySystemComponent; }
	const UElementCharacterAttributeSet* GetCharacterAttributes() const { return CharacterAttributes; }
	UInventoryComponent* GetInventoryComponent() const { return InventoryComponent; }

private:
	UPROPERTY(VisibleAnywhere, Category="Abilities")
	TObjectPtr<UElementAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, Category="Abilities")
	TObjectPtr<UElementCharacterAttributeSet> CharacterAttributes;

	UPROPERTY(VisibleAnywhere, Category="Inventory")
	TObjectPtr<UInventoryComponent> InventoryComponent;
};
