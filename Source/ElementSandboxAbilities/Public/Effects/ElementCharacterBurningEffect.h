#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"

#include "ElementCharacterBurningEffect.generated.h"

/**
 * State.Burning 的唯一投影。燃烧时长由 Fire ECS 持有；GE 只保存层数和连续 DOT 周期。
 */
UCLASS()
class ELEMENTSANDBOXABILITIES_API UElementCharacterBurningEffect final
	: public UGameplayEffect
{
	GENERATED_BODY()

public:
	UElementCharacterBurningEffect();

	/** 在任何 Effect 应用前，由唯一 Fire RuleSet 在 Game Thread 冻结一次。 */
	static bool ConfigureRuntimeRules(float BaseDamage, float Period);
	static float GetConfiguredBaseDamagePerPeriod();
	static float GetConfiguredPeriodSeconds();

	static constexpr float BaseDamagePerPeriod = 1.0f;
	static constexpr float PeriodSeconds = 0.5f;
};
