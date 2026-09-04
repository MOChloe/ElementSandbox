#include "Game/WorldStreamingHUDPresenterComponent.h"

#include "GameFramework/PlayerController.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TimerManager.h"
#include "Tree/SettlementTreeCollisionWorldSubsystem.h"
#include "Tree/SettlementTreePresentationWorldSubsystem.h"
#include "Tree/SettlementTreeWorldSubsystem.h"
#include "WorldObjectRuntimeStats.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"
#include "WorldStreaming/WorldStreamingHUDWidget.h"

CSV_DEFINE_CATEGORY(WorldStreamingHUD, true);

UWorldStreamingHUDPresenterComponent::UWorldStreamingHUDPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UWorldStreamingHUDPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	EnsureInitialized();
}

void UWorldStreamingHUDPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimerHandle);
	}
	// Viewport 不会替旧 Controller 自动移除 Widget；ClientTravel 前必须显式清理。
	if (Widget)
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}
	StreamingEndpoint = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UWorldStreamingHUDPresenterComponent::EnsureInitialized()
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalPlayerController() || !Controller->GetLocalPlayer())
	{
		return;
	}
	if (!StreamingEndpoint)
	{
		StreamingEndpoint = Controller->FindComponentByClass<UWorldChunkStreamingComponent>();
	}
	if (!Widget)
	{
		Widget = CreateWidget<UWorldStreamingHUDWidget>(Controller, UWorldStreamingHUDWidget::StaticClass());
		if (Widget)
		{
			// Activation Core Ready 前遮住未注入世界；高于其余 HUD，Ready 后内部折叠。
			Widget->AddToViewport(1000);
		}
	}
	if (UWorld* World = GetWorld();
		World && Widget && StreamingEndpoint && !World->GetTimerManager().IsTimerActive(RefreshTimerHandle))
	{
		World->GetTimerManager().SetTimer(RefreshTimerHandle, this,
										  &UWorldStreamingHUDPresenterComponent::RefreshMetrics, 0.25f, true, 0.0f);
	}
}

void UWorldStreamingHUDPresenterComponent::RefreshMetrics()
{
	if (!Widget || !StreamingEndpoint)
	{
		return;
	}
	UWorld* World = GetWorld();
	UWorldStorageSubsystem* Storage = World ? World->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!Storage)
	{
		return;
	}

	const FWorldStorageRuntimeStats StorageStats = Storage->GetRuntimeStats();
	const FWorldChunkStreamingStats StreamingStats = StreamingEndpoint->GetStreamingStats();
	FWorldStreamingHUDMetrics Metrics;
	Metrics.CompleteStructureCount = StreamingStats.CompleteStructureCount > 0 ? StreamingStats.CompleteStructureCount
																			   : StorageStats.CompleteStructureCount;
	Metrics.BuildingEntityCount =
		StreamingStats.BuildingEntityCount > 0 ? StreamingStats.BuildingEntityCount : StorageStats.BuildingEntityCount;
	Metrics.WorldObjectEntityCount = StreamingStats.WorldObjectEntityCount > 0 ? StreamingStats.WorldObjectEntityCount
																			   : StorageStats.WorldObjectEntityCount;
	Metrics.ResidentEntityCount = StorageStats.ResidentEntityCount;
	Metrics.ResidentChunkCount = StorageStats.ResidentChunkCount;
	Metrics.AuthorityResidentEntityCount = StreamingStats.AuthorityResidentEntityCount;
	Metrics.AuthorityResidentChunkCount = StreamingStats.AuthorityResidentChunkCount;
	Metrics.PendingLoadCount = StorageStats.PendingLoadCount;
	Metrics.PendingInjectionCount = StorageStats.PendingInjectionCount;
	Metrics.AuthorityPendingLoadCount = StreamingStats.AuthorityPendingLoadCount;
	Metrics.AuthorityPendingInjectionCount = StreamingStats.AuthorityPendingInjectionCount;
	Metrics.DirtyEntityCount = StreamingStats.AuthorityDirtyEntityCount;
	Metrics.OfferedChunkCount = StreamingStats.OfferedChunkCount;
	Metrics.AcknowledgedChunkCount = StreamingStats.AcknowledgedChunkCount;
	Metrics.ActivationCoreChunkCount = StreamingStats.ActivationCoreChunkCount;
	Metrics.ActivationCoreAcknowledgedChunkCount = StreamingStats.ActivationCoreAcknowledgedChunkCount;
	Metrics.ActivationCoreAuthorityReadyChunkCount = StreamingStats.ActivationCoreAuthorityReadyChunkCount;
	Metrics.InterestCenterX = StreamingStats.InterestCenter.X;
	Metrics.InterestCenterY = StreamingStats.InterestCenter.Y;
	Metrics.InterestCenterZ = StreamingStats.InterestCenter.Z;
	Metrics.NetworkPendingChunkCount = StreamingStats.PendingChunkCount;
	Metrics.NetworkPendingLiveDeltaCount = StreamingStats.PendingLiveDeltaCount;
	Metrics.SegmentsInFlight = StreamingStats.SegmentsInFlight;
	Metrics.CacheHitCount = StorageStats.CacheHitCount;
	Metrics.CacheMissCount = StorageStats.CacheMissCount;
	Metrics.PayloadBytesReceived = StreamingStats.PayloadBytesReceived;
	Metrics.PayloadBytesSent = StreamingStats.PayloadBytesSent;
	Metrics.AuthorityAwakePhysicsPinnedEntityCount = StreamingStats.AuthorityAwakePhysicsPinnedEntityCount;
	Metrics.AuthorityOldestAwakePhysicsPinSeconds = StreamingStats.AuthorityOldestAwakePhysicsPinSeconds;
	Metrics.WorldSimulationTimeMilliseconds = StreamingStats.WorldSimulationTimeMilliseconds > 0
												  ? StreamingStats.WorldSimulationTimeMilliseconds
												  : StorageStats.WorldSimulationTimeMilliseconds;
	Metrics.bActivationCoreReady = StreamingStats.bActivationCoreReady;
	Metrics.bCheckpointInFlight = StreamingStats.bCheckpointInFlight;

	if (const UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		Metrics.WorldObjectResidentEntityCount = WorldObjects->GetRuntimeStats().EntityCount;
	}
	if (const USettlementTreeWorldSubsystem* Trees = World->GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		Metrics.TreeResidentCount = Trees->GetStats().ResidentTreeCount;
	}
	if (const USettlementTreePresentationWorldSubsystem* TreePresentation =
			World->GetSubsystem<USettlementTreePresentationWorldSubsystem>())
	{
		const FSettlementTreePresentationStats TreeStats = TreePresentation->GetStats();
		Metrics.TreeActiveCount = TreeStats.ActiveCount;
		Metrics.TreeTransitionCount = TreeStats.TransitionCount;
		Metrics.TreeGraceCount = TreeStats.GraceCount;
		Metrics.TreePendingCount = TreeStats.PendingCount;
		Metrics.TreeHISMCellCount = TreeStats.HISMCellCount;
		Metrics.TreeInstanceCount = TreeStats.InstanceCount;
		Metrics.TreeRenderHostCount = TreeStats.RenderHostCount;
		Metrics.TreeBuildCount = TreeStats.TreeBuildCount;
		Metrics.TreeCoalescedBuildCount = TreeStats.CoalescedTreeBuildCount;
		Metrics.TreeSelectionMilliseconds = TreeStats.LastSelectionMilliseconds;
		Metrics.TreeApplyMilliseconds = TreeStats.LastApplyMilliseconds;
		Metrics.TreeBuildMilliseconds = TreeStats.LastTreeBuildScheduleMilliseconds;
		Metrics.TreeLocalSelectionPassCount = TreeStats.LocalSelectionPassCount;
		Metrics.TreeFarSelectionPassCount = TreeStats.FarSelectionPassCount;
		Metrics.TreeWorkerDispatchCount = TreeStats.WorkerDispatchCount;
		Metrics.TreeWorkerDiscardCount = TreeStats.WorkerDiscardCount;
		Metrics.TreeCandidateTestCount = TreeStats.CandidateTestCount;
		Metrics.TreeCellDeltaEvaluationCount = TreeStats.CellDeltaEvaluationCount;
		Metrics.TreeHISMAddCount = TreeStats.HISMAddCount;
		Metrics.TreeHISMRemoveCount = TreeStats.HISMRemoveCount;
		Metrics.TreeInvalidVisibleRemovalCount = TreeStats.InvalidVisibleRemovalCount;
	}
	if (const USettlementTreeCollisionWorldSubsystem* TreeCollision =
			World->GetSubsystem<USettlementTreeCollisionWorldSubsystem>())
	{
		const FSettlementTreeCollisionStats CollisionStats = TreeCollision->GetStats();
		Metrics.TreeCollisionCount = CollisionStats.CollisionInstanceCount;
		Metrics.TreeCollisionSourceSubmitCount = CollisionStats.SourceSubmitCount;
		Metrics.TreeCollisionCatalogQueryCount = CollisionStats.CatalogQueryCount;
		Metrics.TreeCollisionCandidateTestCount = CollisionStats.CandidateTestCount;
	}
	// 与 HUD 使用同一份只读采样，区分上游 Chunk 进度和已注入树木的 HISM 装填。
	// Sample 标识本次刷新所在帧；CSV 分析不能把两次刷新之间的空值当作计数归零。
	CSV_CUSTOM_STAT(WorldStreamingHUD, Sample, 1, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ClientWorldSeconds, World->GetTimeSeconds(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ClientResidentEntities, Metrics.ResidentEntityCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ClientResidentChunks, Metrics.ResidentChunkCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ServerResidentEntities, Metrics.AuthorityResidentEntityCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ServerResidentChunks, Metrics.AuthorityResidentChunkCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ClientPendingLoads, Metrics.PendingLoadCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ClientPendingInjections, Metrics.PendingInjectionCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ServerPendingLoads, Metrics.AuthorityPendingLoadCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, ServerPendingInjections, Metrics.AuthorityPendingInjectionCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, OfferedChunks, Metrics.OfferedChunkCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, AcknowledgedChunks, Metrics.AcknowledgedChunkCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, NetworkPendingChunks, Metrics.NetworkPendingChunkCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, CacheHits, static_cast<double>(Metrics.CacheHitCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, CacheMisses, static_cast<double>(Metrics.CacheMissCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, PayloadReceivedBytes, static_cast<double>(Metrics.PayloadBytesReceived), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeResidents, Metrics.TreeResidentCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeInstances, Metrics.TreeInstanceCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreePending, Metrics.TreePendingCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeCells, Metrics.TreeHISMCellCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeBuilds, static_cast<double>(Metrics.TreeBuildCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeSelectionMilliseconds, Metrics.TreeSelectionMilliseconds, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeApplyMilliseconds, Metrics.TreeApplyMilliseconds, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeBuildScheduleMilliseconds, Metrics.TreeBuildMilliseconds, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeWorkerDispatches, static_cast<double>(Metrics.TreeWorkerDispatchCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeWorkerDiscards, static_cast<double>(Metrics.TreeWorkerDiscardCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeAdds, static_cast<double>(Metrics.TreeHISMAddCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WorldStreamingHUD, TreeRemoves, static_cast<double>(Metrics.TreeHISMRemoveCount), ECsvCustomStatOp::Set);
	Widget->SetMetrics(Metrics);
}
