#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Processing/BuildProcessor.h"

class UBuildingWorldSubsystem;

/** Authority DoorState 改变且对应 Part Transform 已成功提交后的低频通知。 */
DECLARE_DELEGATE_OneParam(FBuildDoorStateChangedDelegate, FBuildEntityHandle);

/** 只处理已提交 Door 的活跃队列，不扫描 DoorState Fragment Pool。 */
class ELEMENTSANDBOXBUILDINGCATALOG_API FBuildDoorProcessor final
	: public FBuildProcessor
{
public:
	explicit FBuildDoorProcessor(
		bool bInAcceptAuthorityInteractions = true,
		FBuildDoorStateChangedDelegate InStateChanged = {});

	/** 只接受一个尚未活跃的完整 Entity Handle；成功时唤醒 World Scheduler。 */
	bool RequestInteraction(FBuildEntityHandle Entity);
	/** 纯客户端收到新 DoorState 后唤醒本地派生动画；不会生成权威命令。 */
	/** Chunk Restore/Upsert 后重建门的派生 Part Transform，并继续未完成过渡。 */
	bool NotifyRestoredState(FBuildEntityHandle Entity);

	/** 供 Door 自身的动画和聚焦提示共享同一铰链运动。 */
	static FTransform CalculateDoorMotion(float OpenFraction);

private:
	struct FDoorWork final
	{
		FBuildEntityHandle Entity;
		bool bInteractionPending = true;
		bool bMotionActive = false;
		bool bStateChangePending = false;
	};

	virtual EBuildProcessorRunResult Execute(FBuildProcessorContext& Context) override;
	bool AdvanceDoor(
		UBuildingWorldSubsystem& BuildingSubsystem,
		FDoorWork& Work,
		double CurrentServerTimeSeconds,
		bool& bOutFinished);
	static bool ApplyDoorPartTransforms(
		UBuildingWorldSubsystem& BuildingSubsystem,
		FBuildEntityHandle Entity,
		float OpenFraction);

	TArray<FDoorWork> ActiveWork;
	TSet<FBuildEntityHandle> ActiveEntities;
	FBuildDoorStateChangedDelegate StateChanged;
	bool bAcceptAuthorityInteractions = true;
};
