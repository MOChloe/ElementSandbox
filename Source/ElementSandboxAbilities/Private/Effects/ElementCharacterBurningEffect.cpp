#include "Effects/ElementCharacterBurningEffect.h"

#include "Attributes/ElementCharacterAttributeSet.h"
#include "Effects/ElementFireDamageExecution.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "Tags/ElementGameplayTags.h"

namespace
{
	float ConfiguredBaseDamagePerPeriod = UElementCharacterBurningEffect::BaseDamagePerPeriod;
	float ConfiguredPeriodSeconds = UElementCharacterBurningEffect::PeriodSeconds;
}

UElementCharacterBurningEffect::UElementCharacterBurningEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	Period = FScalableFloat(PeriodSeconds);
	bExecutePeriodicEffectOnApplication = false;

	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	StackLimitCount = 3;
	StackDurationRefreshPolicy =
		EGameplayEffectStackingDurationPolicy::NeverRefresh;
	StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
	StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::ClearEntireStack;

	UTargetTagsGameplayEffectComponent* TargetTags =
		CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("BurningTargetTags"));
	GEComponents.Add(TargetTags);
	FInheritedTagContainer BurningTags;
	BurningTags.AddTag(ElementSandboxGameplayTags::State_Burning);
	TargetTags->SetAndApplyTargetTagChanges(BurningTags);

	FGameplayEffectExecutionDefinition& Damage = Executions.AddDefaulted_GetRef();
	Damage.CalculationClass = UElementFireDamageExecution::StaticClass();
}

bool UElementCharacterBurningEffect::ConfigureRuntimeRules(
	const float BaseDamage,
	const float InPeriod)
{
	check(IsInGameThread());
	if (!FMath::IsFinite(BaseDamage) || BaseDamage <= 0.0f
		|| !FMath::IsFinite(InPeriod) || InPeriod <= 0.0f)
	{
		return false;
	}
	ConfiguredBaseDamagePerPeriod = BaseDamage;
	ConfiguredPeriodSeconds = InPeriod;
	GetMutableDefault<UElementCharacterBurningEffect>()->Period =
		FScalableFloat(ConfiguredPeriodSeconds);
	return true;
}

float UElementCharacterBurningEffect::GetConfiguredBaseDamagePerPeriod()
{
	return ConfiguredBaseDamagePerPeriod;
}

float UElementCharacterBurningEffect::GetConfiguredPeriodSeconds()
{
	return ConfiguredPeriodSeconds;
}
