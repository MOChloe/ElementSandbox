#pragma once

#include "CoreMinimal.h"
#include "Items/EquippedAbilityItemFeature.h"

#include "AxeSwingAbilityFeature.generated.h"

/** 斧头装备时授予服务器权威破坏 Ability。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UAxeSwingAbilityFeature final : public UEquippedAbilityItemFeature
{
	GENERATED_BODY()

public:
	UAxeSwingAbilityFeature();
};
