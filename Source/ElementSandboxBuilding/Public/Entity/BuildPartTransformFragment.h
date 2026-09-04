#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"

#include "BuildPartTransformFragment.generated.h"

/**
 * 需要独立移动 Render Part 的 Entity 才持有的每实例局部 Transform。
 * 数组必须与共享 Definition.MeshParts 使用相同 PartId 和长度。
 * 它由每个 World 的 Processor 派生，不属于网络同步状态。
 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildPartTransformFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FTransform> LocalTransforms;

	/** 最近一次成功提交的局部位姿；延迟初始化时以 Definition 为基线。 */
	UPROPERTY()
	TArray<FTransform> CommittedLocalTransforms;

	/** 与 LocalTransforms 使用相同 PartId；只推进实际变化的 Part。 */
	UPROPERTY()
	TArray<uint64> Revisions;
};
