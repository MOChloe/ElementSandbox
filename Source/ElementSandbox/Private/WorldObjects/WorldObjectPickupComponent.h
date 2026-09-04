#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Entity/WorldEntityId.h"
#include "WorldObjects/WorldObjectPickupFailure.h"
#include "WorldObjectPickupComponent.generated.h"

class APawn;

/** 本地拾取输入和请求生命周期；不预测背包增加，不在客户端销毁世界实体。 */
UCLASS(NotBlueprintable, ClassGroup=(WorldObject))
class UWorldObjectPickupComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldObjectPickupComponent();
	/** E 按下时执行当前操作；只有拾取/空目标可以进入持续收集。 */
	bool BeginInteract();
	void EndInteract();
	bool IsCollecting() const { return bCollecting; }
	/** RPC 门面先占用唯一在途请求；已确认但还未收到 Tombstone 的身份也拒绝重发。 */
	bool TryBeginRequest(FWorldEntityId WorldEntityId);
	void CompleteRequest(FWorldEntityId WorldEntityId, EWorldObjectPickupFailure Failure);
	bool IsTargetUnavailable(FWorldEntityId WorldEntityId) const;
	bool TryGetFeedback(FText& OutText) const;
	/** Tick 与确定性输入测试共用，时间来自本地 World，不依赖 Authority ECS 节拍。 */
	void AdvanceCollection(double NowSeconds);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void PruneCompletedTargets();
	double GetNow() const;

	TWeakObjectPtr<APawn> CollectingPawn;
	FWorldEntityId PendingTarget;
	TSet<FWorldEntityId> AwaitingTombstones;
	FWorldEntityId RejectedTarget;
	double RejectedUntil = 0.0;
	double NextRepeatTime = 0.0;
	double FeedbackUntil = 0.0;
	FText Feedback;
	bool bCollecting = false;
};
