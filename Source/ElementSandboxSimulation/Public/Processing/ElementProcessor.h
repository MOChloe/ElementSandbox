#pragma once

#include "CoreMinimal.h"
#include "Entity/ElementEntityRegistry.h"
#include "Runtime/ElementRuntimeTypes.h"

/** Numeric Processor 在 Game Thread 冻结源 Fragment，随后只消费纯值查询统计并输出线程本地 Offset。 */
class ELEMENTSANDBOXSIMULATION_API FElementNumericProcessor
{
public:
	virtual ~FElementNumericProcessor() = default;
	virtual const FElementProcessorDescriptor& GetDescriptor() const = 0;
	virtual bool CaptureInfluence(
		const FElementEntityRegistry& Registry,
		FElementEntityHandle Source,
		FElementInfluenceSnapshot& OutSnapshot) const = 0;
	virtual void Execute(
		TConstArrayView<FElementQueryStatistics> Statistics,
		TArray<FElementOffset>& OutOffsets) const = 0;
};

/** State Processor 唯一拥有一个状态 Channel；它只在 Offset 归并完成后读取最终数值。 */
class ELEMENTSANDBOXSIMULATION_API FElementStateProcessor
{
public:
	virtual ~FElementStateProcessor() = default;
	virtual const FElementProcessorDescriptor& GetDescriptor() const = 0;
	virtual bool Execute(
		const FElementStateProcessorInput& Input,
		FElementStateProcessorOutput& OutOutput) const = 0;
};

