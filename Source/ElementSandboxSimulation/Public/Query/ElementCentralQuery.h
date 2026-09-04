#pragma once

#include "CoreMinimal.h"
#include "Runtime/ElementRuntimeTypes.h"

/** 中央查询层的无状态窄相入口；调用方负责 BVH 粗筛与 Snapshot Handle 解析。 */
class ELEMENTSANDBOXSIMULATION_API FElementCentralQuery final
{
public:
	/** 同一逻辑 Source 的复合 Shape 在内部去重，最多输出一条统计。 */
	static bool EvaluateMotion(
		const FElementMotionSubmission& Motion,
		const FElementTargetSnapshot& Target,
		const FElementInfluenceSnapshot& Influence,
		EElementSpatialWeightMode WeightMode,
		FElementQueryStatistics& OutStatistics);

	/** Source 新增/移动/删除后，对静止 Target 重新计算当前瞬时权重。 */
	static bool EvaluateStatic(
		const FElementTargetSnapshot& Target,
		const FElementInfluenceSnapshot& Influence,
		EElementSpatialWeightMode WeightMode,
		int64 WorldTimeMilliseconds,
		FElementQueryStatistics& OutStatistics);
};

