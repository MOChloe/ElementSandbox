#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "WoodProductPresentationSettings.generated.h"

/** 普通木块/木炭 HISM 的独立分区与原生提交预算。 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Wood Product Presentation Settings"))
class ELEMENTSANDBOXWORLDOBJECTCATALOG_API UWoodProductPresentationSettings final
	: public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** 每个 HISM Cell 的边长；避免一次异步 Tree Apply 重排整场全部木块。 */
	UPROPERTY(Config, EditAnywhere, Category = "Partition", meta = (ClampMin = "10000.0", Units = "cm"))
	double CellSize = 100000.0;
	/** 单次不可抢占 AddInstances/RemoveInstances 的最大实例数。 */
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "1"))
	int32 MaximumNativeInstanceBatchSize = 512;
	/** 收集与轻量映射处理达到目标后，不再开始下一组原生提交。 */
	UPROPERTY(Config, EditAnywhere, Category = "Budget", meta = (ClampMin = "0.1", Units = "ms"))
	double InstanceApplyTargetMilliseconds = 1.0;
	/** 最后一次编辑后的静默时间；期间只使用增量实例缓冲，不反复重建 Cluster Tree。 */
	UPROPERTY(Config, EditAnywhere, Category = "HISM", meta = (ClampMin = "0.0", Units = "s"))
	double TreeBuildQuietSeconds = 0.25;
	UPROPERTY(Config, EditAnywhere, Category = "HISM", meta = (ClampMin = "0.0", Units = "s"))
	double TreeBuildMaxDeferralSeconds = 1.0;
};
