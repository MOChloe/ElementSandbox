#pragma once

#include "CoreMinimal.h"
#include "Items/EquippedAbilityItemFeature.h"

#include "MeteorStrikeAbilityFeature.generated.h"

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UMeteorStrikeAbilityFeature final : public UEquippedAbilityItemFeature
{
	GENERATED_BODY()
public:
	UMeteorStrikeAbilityFeature();
};
