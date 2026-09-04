#pragma once

#include "CoreMinimal.h"
#include "Items/EquippedAbilityItemFeature.h"

#include "DemolitionToolSwingAbilityFeature.generated.h"

/** 拆除锤装备时只授予它自己的挥击表现 Ability。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UDemolitionToolSwingAbilityFeature final : public UEquippedAbilityItemFeature
{
	GENERATED_BODY()

public:
	UDemolitionToolSwingAbilityFeature();
};
