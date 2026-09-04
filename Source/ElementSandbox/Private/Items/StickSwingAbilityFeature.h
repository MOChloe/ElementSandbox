#pragma once

#include "CoreMinimal.h"
#include "Items/EquippedAbilityItemFeature.h"
#include "StickSwingAbilityFeature.generated.h"

/** 木棍的装备能力配置；只声明“可挥动”，不声明命中后果。 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class UStickSwingAbilityFeature final : public UEquippedAbilityItemFeature
{
	GENERATED_BODY()

public:
	UStickSwingAbilityFeature();
};
