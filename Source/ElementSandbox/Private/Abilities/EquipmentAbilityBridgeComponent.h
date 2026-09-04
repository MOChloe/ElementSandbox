#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayAbilitySpec.h"
#include "EquipmentAbilityBridgeComponent.generated.h"

class UElementAbilitySystemComponent;
class UEquipmentComponent;
class UItemInstance;

/**
 * ElementSandbox 装配层的 Items → GAS 适配器。
 * Items 模块只广播装备变化，Abilities 模块只处理 AbilitySpec；二者互不依赖。
 */
UCLASS()
class UEquipmentAbilityBridgeComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UEquipmentAbilityBridgeComponent();

	/** Character 建立或清除 ASC Avatar 时调用；重复初始化不会累积 Ability。 */
	void InitializeAbilitySystem(UElementAbilitySystemComponent* NewAbilitySystem);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UElementAbilitySystemComponent> AbilitySystem;

	UPROPERTY(Transient)
	TObjectPtr<UEquipmentComponent> BoundEquipment;

	UPROPERTY(Transient)
	TObjectPtr<UItemInstance> GrantedSourceItem;

	TArray<FGameplayAbilitySpecHandle> GrantedAbilityHandles;

	void EnsureEquipmentBinding();
	void HandleEquippedItemChanged(UItemInstance* PreviousItem, UItemInstance* NewItem);
	void GrantAbilitiesFromItem(UItemInstance* ItemInstance);
	void RevokeGrantedAbilities();
};
