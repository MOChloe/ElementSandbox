#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/ElementAbilitySet.h"
#include "Item/ItemFeature.h"
#include "EquippedAbilityItemFeature.generated.h"

/** 道具被装备期间授予角色的 Ability 集；实际 grant/revoke 由装配层桥接组件执行。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UEquippedAbilityItemFeature : public UItemFeature
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	const FElementAbilitySet& GetAbilitySet() const { return AbilitySet; }

protected:
	UPROPERTY(EditDefaultsOnly, Replicated, Category="Abilities")
	FElementAbilitySet AbilitySet;
};
