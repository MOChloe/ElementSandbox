#pragma once

#include "CoreMinimal.h"
#include "Items/EquippedAbilityItemFeature.h"

#include "FireballAbilityFeature.generated.h"

/** 火焰球装备时授予主要投掷 Ability。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UFireballAbilityFeature final : public UEquippedAbilityItemFeature
{
	GENERATED_BODY()

public:
	UFireballAbilityFeature();
};
