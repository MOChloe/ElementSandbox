#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "WorldStreamingHUDWidget.generated.h"

class UBorder;
class UProgressBar;
class UTextBlock;

/** 首次进入世界时遮罩所处的纯表现状态。 */
enum class EWorldStreamingLoadingState : uint8
{
	Connecting,
	Loading,
	Ready
};

/** UI 只接收装配层提交的纯值指标，不读取 WorldStorage 或网络对象。 */
struct ELEMENTSANDBOXUI_API FWorldStreamingHUDMetrics final
{
	int64 CompleteStructureCount = 0;
	int64 BuildingEntityCount = 0;
	int64 WorldObjectEntityCount = 0;
	int32 WorldObjectResidentEntityCount = 0;
	int32 TreeResidentCount = 0;
	int32 TreeActiveCount = 0;
	int32 TreeTransitionCount = 0;
	int32 TreeGraceCount = 0;
	int32 TreePendingCount = 0;
	int32 TreeHISMCellCount = 0;
	int32 TreeInstanceCount = 0;
	int32 TreeRenderHostCount = 0;
	int64 TreeBuildCount = 0;
	int64 TreeCoalescedBuildCount = 0;
	int32 TreeCollisionCount = 0;
	double TreeSelectionMilliseconds = 0.0;
	double TreeApplyMilliseconds = 0.0;
	double TreeBuildMilliseconds = 0.0;
	int64 TreeLocalSelectionPassCount = 0;
	int64 TreeFarSelectionPassCount = 0;
	int64 TreeWorkerDispatchCount = 0;
	int64 TreeWorkerDiscardCount = 0;
	int64 TreeCandidateTestCount = 0;
	int64 TreeCellDeltaEvaluationCount = 0;
	int64 TreeHISMAddCount = 0;
	int64 TreeHISMRemoveCount = 0;
	int64 TreeInvalidVisibleRemovalCount = 0;
	int64 TreeCollisionSourceSubmitCount = 0;
	int64 TreeCollisionCatalogQueryCount = 0;
	int64 TreeCollisionCandidateTestCount = 0;
	int32 ResidentEntityCount = 0;
	int32 ResidentChunkCount = 0;
	int32 AuthorityResidentEntityCount = 0;
	int32 AuthorityResidentChunkCount = 0;
	int32 PendingLoadCount = 0;
	int32 PendingInjectionCount = 0;
	int32 AuthorityPendingLoadCount = 0;
	int32 AuthorityPendingInjectionCount = 0;
	int32 DirtyEntityCount = 0;
	int32 OfferedChunkCount = 0;
	int32 AcknowledgedChunkCount = 0;
	int32 ActivationCoreChunkCount = 0;
	int32 ActivationCoreAcknowledgedChunkCount = 0;
	int32 ActivationCoreAuthorityReadyChunkCount = 0;
	int32 InterestCenterX = 0;
	int32 InterestCenterY = 0;
	int32 InterestCenterZ = 0;
	int32 NetworkPendingChunkCount = 0;
	int32 NetworkPendingLiveDeltaCount = 0;
	int32 SegmentsInFlight = 0;
	uint64 CacheHitCount = 0;
	uint64 CacheMissCount = 0;
	int64 PayloadBytesReceived = 0;
	int64 PayloadBytesSent = 0;
	int64 WorldSimulationTimeMilliseconds = 0;
	int32 AuthorityAwakePhysicsPinnedEntityCount = 0;
	double AuthorityOldestAwakePhysicsPinSeconds = 0.0;
	bool bActivationCoreReady = false;
	bool bCheckpointInFlight = false;
};

/** 百万结构演示的常驻只读指标面板。 */
UCLASS()
class ELEMENTSANDBOXUI_API UWorldStreamingHUDWidget final : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetMetrics(const FWorldStreamingHUDMetrics& InMetrics);
	const FWorldStreamingHUDMetrics& GetMetrics() const { return Metrics; }
	EWorldStreamingLoadingState GetLoadingState() const { return LoadingState; }
	float GetLoadingProgress() const { return LoadingProgress; }

protected:
	virtual void NativeOnInitialized() override;

private:
	void Refresh();
	void RefreshLoadingCurtain();

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> MetricsText;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> LoadingCurtain;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LoadingStatusText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LoadingDetailText;

	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> LoadingProgressBar;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> LoadingProgressText;

	FWorldStreamingHUDMetrics Metrics;
	EWorldStreamingLoadingState LoadingState = EWorldStreamingLoadingState::Connecting;
	float LoadingProgress = 0.0f;
};
