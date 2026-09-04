#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "PresentationSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Presentation"))
class ELEMENTSANDBOXPRESENTATION_API UPresentationSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

  public:
	/** 最后一次 HISM 实例编辑后，等待该静默时长再构建 Cluster Tree。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh Pool", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float HISMTreeBuildQuietSeconds = 0.25f;

	/** 连续实例编辑时允许 Cluster Tree 延后的最长时间。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh Pool", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float HISMTreeBuildMaxDeferralSeconds = 1.0f;

	/** 小型交互 Cluster 的建树成本很低，实例变更后同帧发布，避免排队等待大世界异步树。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh Pool", meta = (ClampMin = "0", ClampMax = "1024"))
	int32 HISMSynchronousBuildMaxInstances = 64;

	/** 单次不可抢占原生提交的实例上限；同帧可在耗时预算内连续提交多个 Cluster 批次。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh Pool", meta = (ClampMin = "1", ClampMax = "16384"))
	int32 MaximumNativeInstanceBatchSize = 1024;

	/** 普通 Tick 的实例合批与提交墙钟预算。至少推进一批；显式 FlushNow 排空待办。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh Pool", meta = (ClampMin = "0.0", ClampMax = "16.0"))
	double InstanceApplyTargetMilliseconds = 2.0;
};
