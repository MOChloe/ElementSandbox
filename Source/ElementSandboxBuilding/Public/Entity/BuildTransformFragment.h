#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"

#include "BuildTransformFragment.generated.h"

/** Building Entity 的权威世界位姿；空间索引与场景表现都只能把它作为派生输入。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDING_API FBuildTransformFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FTransform WorldTransform = FTransform::Identity;

	/** 最近一次成功提交到空间、碰撞、表现和中性查询快照流的位姿。 */
	UPROPERTY()
	FTransform CommittedWorldTransform = FTransform::Identity;

	/** 只在真实 Entity Transform 成功提交后推进；0 永远无效。 */
	UPROPERTY()
	uint64 Revision = 1;
};
