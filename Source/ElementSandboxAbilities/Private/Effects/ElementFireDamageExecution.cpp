#include "Effects/ElementFireDamageExecution.h"

#include "Attributes/ElementCharacterAttributeSet.h"
#include "Effects/ElementCharacterBurningEffect.h"

namespace
{
	struct FElementFireDamageCaptures final
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(FireResistance);

		FElementFireDamageCaptures()
		{
			DEFINE_ATTRIBUTE_CAPTUREDEF(
				UElementCharacterAttributeSet, FireResistance, Target, false);
		}
	};

	const FElementFireDamageCaptures& GetFireDamageCaptures()
	{
		static const FElementFireDamageCaptures Captures;
		return Captures;
	}
}

UElementFireDamageExecution::UElementFireDamageExecution()
{
	RelevantAttributesToCapture.Add(
		GetFireDamageCaptures().FireResistanceDef);
}

void UElementFireDamageExecution::Execute_Implementation(
	const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	FAggregatorEvaluateParameters Evaluation;
	Evaluation.SourceTags = ExecutionParams.GetOwningSpec().CapturedSourceTags.GetAggregatedTags();
	Evaluation.TargetTags = ExecutionParams.GetOwningSpec().CapturedTargetTags.GetAggregatedTags();
	float Resistance = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		GetFireDamageCaptures().FireResistanceDef, Evaluation, Resistance);
	Resistance = FMath::Clamp(Resistance, 0.0f, 1.0f);
	const float Damage = UElementCharacterBurningEffect::GetConfiguredBaseDamagePerPeriod()
		* FMath::Square(1.0f - Resistance);
	OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
		UElementCharacterAttributeSet::GetIncomingDamageAttribute(),
		EGameplayModOp::Additive,
		Damage));
}
