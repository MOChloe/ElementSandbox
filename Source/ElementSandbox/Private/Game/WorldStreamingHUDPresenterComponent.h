#pragma once

#include "Components/ActorComponent.h"

#include "WorldStreamingHUDPresenterComponent.generated.h"

class UWorldChunkStreamingComponent;
class UWorldStreamingHUDWidget;

/** 低频汇总 WorldStorage/Chunk/Tree 指标并投影到 Owner HUD；不参与协议正确性。 */
UCLASS(ClassGroup = (UI))
class UWorldStreamingHUDPresenterComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldStreamingHUDPresenterComponent();

	/** PlayerController BeginPlay 的显式重试入口，处理组件 BeginPlay 早于 LocalPlayer 就绪的情况。 */
	void EnsureInitialized();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void RefreshMetrics();

	UPROPERTY(Transient)
	TObjectPtr<UWorldStreamingHUDWidget> Widget;

	UPROPERTY(Transient)
	TObjectPtr<UWorldChunkStreamingComponent> StreamingEndpoint;

	FTimerHandle RefreshTimerHandle;
};
