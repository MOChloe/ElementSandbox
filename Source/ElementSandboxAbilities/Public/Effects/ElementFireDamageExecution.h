#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"

#include "ElementFireDamageExecution.generated.h"

/** 每个 Burning 周期按 FireResistance 平方减伤输出 IncomingDamage。 */
UCLASS()
class ELEMENTSANDBOXABILITIES_API UElementFireDamageExecution final
	: public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UElementFireDamageExecution();

	virtual void Execute_Implementation(
		const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
