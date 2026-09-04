#include "Performance/SettlementTreePerformanceWorldSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/MovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RHIStats.h"
#include "Tree/SettlementTreeCollisionWorldSubsystem.h"
#include "Tree/SettlementTreePresentationWorldSubsystem.h"
#include "Tree/SettlementTreeWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSettlementTreePerformance, Log, All);

namespace
{
double Percentile99(const TArray<double>& Values)
{
	if (Values.IsEmpty())
	{
		return 0.0;
	}
	TArray<double> Sorted = Values;
	Sorted.Sort();
	const int32 Index = FMath::Clamp(FMath::CeilToInt(Sorted.Num() * 0.99) - 1, 0, Sorted.Num() - 1);
	return Sorted[Index];
}

FString JsonBool(const bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString SanitizeRunLabel(FString Label)
{
	for (TCHAR& Character : Label)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
		{
			Character = TEXT('_');
		}
	}
	return Label.Left(64);
}
} // namespace

void USettlementTreePerformanceWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FString RequestedLabel;
	if (IsRunningCommandlet() || !FParse::Value(FCommandLine::Get(), TEXT("SettlementTreePerfRun="), RequestedLabel))
	{
		return;
	}
	RunLabel = SanitizeRunLabel(MoveTemp(RequestedLabel));
	if (RunLabel.IsEmpty())
	{
		RunLabel = TEXT("SettlementTrees");
	}
	FParse::Value(FCommandLine::Get(), TEXT("SettlementTreePerfWarmup="), WarmupSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("SettlementTreePerfSample="), SampleDurationSeconds);
	bLoadPhaseOnly = FParse::Param(FCommandLine::Get(), TEXT("SettlementTreePerfLoadOnly"));
	bRouteDuringLoad = FParse::Param(FCommandLine::Get(), TEXT("SettlementTreePerfRouteDuringLoad"));
	WarmupSeconds = FMath::Max(0.0, WarmupSeconds);
	SampleDurationSeconds = FMath::Max(1.0, SampleDurationSeconds);
	OutputDirectory = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling/SettlementTrees")));
	bEnabled = true;
	Phase = ERunPhase::WaitingForClient;
	PhaseStartSeconds = FPlatformTime::Seconds();
}

bool USettlementTreePerformanceWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game;
}

bool USettlementTreePerformanceWorldSubsystem::IsTickable() const
{
	return bEnabled && Phase != ERunPhase::Complete;
}

TStatId USettlementTreePerformanceWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USettlementTreePerformanceWorldSubsystem, STATGROUP_Tickables);
}

void USettlementTreePerformanceWorldSubsystem::Tick(const float DeltaTime)
{
	if (!bEnabled || Phase == ERunPhase::Complete || GetWorldRef().GetNetMode() != NM_Client)
	{
		return;
	}
	APlayerController* Controller = GetWorldRef().GetFirstPlayerController();
	UWorldChunkStreamingComponent* Streaming =
		Controller ? Controller->FindComponentByClass<UWorldChunkStreamingComponent>() : nullptr;
	const double NowSeconds = FPlatformTime::Seconds();
	if (Phase == ERunPhase::WaitingForClient)
	{
		if (!Controller || !Controller->GetPawn() || !Streaming)
		{
			return;
		}
		const FWorldChunkStreamingStats Stats = Streaming->GetStreamingStats();
		if (!Stats.bActivationCoreReady || Stats.ActivationCoreReadySeconds <= 0.0)
		{
			return;
		}
		ActivationCoreSeconds = Stats.ActivationCoreReadySeconds;
			if (bLoadPhaseOnly || bRouteDuringLoad)
			{
				BeginSampling();
				if (bLoadPhaseOnly)
				{
					UE_LOG(LogSettlementTreePerformance, Display,
						   TEXT("%s：Activation Core %.3f 秒；固定观察点采集 %.1f 秒装填阶段。"), *RunLabel,
						   ActivationCoreSeconds, SampleDurationSeconds);
				}
				else
				{
					UE_LOG(LogSettlementTreePerformance, Display,
						   TEXT("%s：Activation Core %.3f 秒；在装填期间执行 %.1f 秒确定性路线。"), *RunLabel,
						   ActivationCoreSeconds, SampleDurationSeconds);
				}
				return;
		}
		Phase = ERunPhase::Warmup;
		PhaseStartSeconds = NowSeconds;
		UE_LOG(LogSettlementTreePerformance, Display, TEXT("%s：Activation Core %.3f 秒；开始 %.1f 秒预热。"),
			   *RunLabel, ActivationCoreSeconds, WarmupSeconds);
		return;
	}
	if (Phase == ERunPhase::Warmup)
	{
		if (NowSeconds - PhaseStartSeconds >= WarmupSeconds)
		{
				Phase = ERunPhase::WaitingForIdleStable;
				IdleStableStartSeconds = 0.0;
				NextIdleWaitDiagnosticSeconds = NowSeconds;
				UE_LOG(LogSettlementTreePerformance, Display,
				   TEXT("%s：预热完成，等待 WorldStorage/Catalog/Tree 全部稳定 2 秒。"), *RunLabel);
		}
		return;
	}
	if (Phase == ERunPhase::WaitingForIdleStable)
	{
		const UWorldStorageSubsystem* Storage = GetWorldRef().GetSubsystem<UWorldStorageSubsystem>();
		const USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>();
		const USettlementTreePresentationWorldSubsystem* Trees =
			GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>();
		const USettlementTreeCollisionWorldSubsystem* Collision =
			GetWorldRef().GetSubsystem<USettlementTreeCollisionWorldSubsystem>();
		const FWorldStorageRuntimeStats StorageStats =
			Storage ? Storage->GetRuntimeStats() : FWorldStorageRuntimeStats{};
			const FWorldChunkStreamingStats StreamingStats =
				Streaming ? Streaming->GetStreamingStats() : FWorldChunkStreamingStats{};
			const FSettlementTreeCatalogStats CatalogStats = Catalog ? Catalog->GetStats() : FSettlementTreeCatalogStats{};
			const FSettlementTreePresentationStats TreeStats =
				Trees ? Trees->GetStats() : FSettlementTreePresentationStats{};
			const FSettlementTreeCollisionStats CollisionStats =
				Collision ? Collision->GetStats() : FSettlementTreeCollisionStats{};
			const bool bTreesIdle = Trees && Trees->IsIdle();
			const bool bCollisionIdle = Collision && Collision->IsIdle();
			const bool bStable =
			Storage && Catalog && Trees && Collision && StorageStats.PendingLoadCount == 0 &&
			StorageStats.PendingInjectionCount == 0
				// Client 本地队列短暂清空不代表外围 Chunk 已结束；必须同时等服务端
				// 当前订阅全部 ACK，否则下一波 Offer 会污染所谓的“真 Idle”。
				&& StreamingStats.PendingChunkCount == 0 && StreamingStats.SegmentsInFlight == 0 &&
				StreamingStats.OfferedChunkCount == StreamingStats.AcknowledgedChunkCount &&
				// Authority Residency 的 8192-Chunk 长尾属于独立服务器负载；Client 性能夹具
				// 只等客户端订阅完整稳定，不能因此空等数十分钟。
				StorageStats.ResidentChunkCount >= StreamingStats.AcknowledgedChunkCount &&
				CatalogStats.Revision == CatalogStats.PublishedRevision && bTreesIdle && bCollisionIdle;
			if (!bStable)
			{
				IdleStableStartSeconds = 0.0;
				if (NowSeconds >= NextIdleWaitDiagnosticSeconds)
				{
					NextIdleWaitDiagnosticSeconds = NowSeconds + 5.0;
					UE_LOG(LogSettlementTreePerformance,
						Display,
						TEXT("%s：Idle 等待诊断；C ResidentChunk=%d Load=%d Inject=%d；")
							TEXT("Net ACK=%d/%d Pending=%d Segments=%d；")
								TEXT("S ResidentChunk=%d Load=%d Inject=%d；")
									TEXT("Catalog Revision=%lld/%lld ResidentTree=%d Cell=%d；")
											TEXT("Tree Idle=%d Pending=%d BuildInFlight=%d；Collision Idle=%d Add=%d Remove=%d。"),
						*RunLabel,
						StorageStats.ResidentChunkCount,
						StorageStats.PendingLoadCount,
						StorageStats.PendingInjectionCount,
						StreamingStats.AcknowledgedChunkCount,
						StreamingStats.OfferedChunkCount,
						StreamingStats.PendingChunkCount,
						StreamingStats.SegmentsInFlight,
						StreamingStats.AuthorityResidentChunkCount,
						StreamingStats.AuthorityPendingLoadCount,
						StreamingStats.AuthorityPendingInjectionCount,
						CatalogStats.Revision,
						CatalogStats.PublishedRevision,
						CatalogStats.ResidentTreeCount,
						CatalogStats.CellCount,
							bTreesIdle ? 1 : 0,
							TreeStats.PendingCount,
							TreeStats.InFlightTreeBuildCount,
							bCollisionIdle ? 1 : 0,
						CollisionStats.PendingAddCount,
						CollisionStats.PendingRemoveCount);
				}
				return;
		}
		if (IdleStableStartSeconds <= 0.0)
		{
			IdleStableStartSeconds = NowSeconds;
		}
		if (NowSeconds - IdleStableStartSeconds >= 2.0)
		{
			BeginIdleSampling();
		}
		return;
	}
	if (Phase == ERunPhase::IdleSampling)
	{
		if (Controller)
		{
			Controller->SetControlRotation(IdleBaseRotation);
		}
		CaptureIdleSample(DeltaTime);
		if (NowSeconds - PhaseStartSeconds >= IdleSampleDurationSeconds)
		{
			EndIdleSampling();
		}
		return;
	}
	if (Phase == ERunPhase::Sampling)
	{
		const double SampleSeconds = NowSeconds - PhaseStartSeconds;
		if (!bLoadPhaseOnly)
		{
			DriveRoute(SampleSeconds, DeltaTime);
		}
		else if (Controller)
		{
			Controller->SetControlRotation(SampleBaseRotation);
		}
		CaptureSample(DeltaTime);
		if (SampleSeconds >= SampleDurationSeconds)
		{
			EndSampling();
		}
		return;
	}
	if (Phase == ERunPhase::Finishing && CsvWriteFuture.IsValid() && CsvWriteFuture.IsReady())
	{
		WriteReportAndExit(CsvWriteFuture.Get());
	}
}

void USettlementTreePerformanceWorldSubsystem::BeginSampling()
{
	APlayerController* Controller = GetWorldRef().GetFirstPlayerController();
	if (!Controller)
	{
		return;
	}
	SampleBaseRotation = Controller->GetControlRotation();
	if (const USettlementTreePresentationWorldSubsystem* Trees =
			GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>())
	{
		const FSettlementTreePresentationStats Stats = Trees->GetStats();
		LastCapturedWorkerDispatchCount = Stats.WorkerDispatchCount;
		LastCapturedHISMChangeCount = Stats.HISMAddCount + Stats.HISMRemoveCount;
	}
	const int32 ReserveFrames = FMath::CeilToInt(SampleDurationSeconds * 120.0);
	FrameMilliseconds.Reserve(ReserveFrames);
	SelectionMilliseconds.Reserve(ReserveFrames);
	ApplyMilliseconds.Reserve(ReserveFrames);
	CatalogPublishMilliseconds.Reserve(ReserveFrames);
	ClientInjectionMilliseconds.Reserve(ReserveFrames);
	AuthorityStepMilliseconds.Reserve(ReserveFrames);
	IFileManager::Get().MakeDirectory(*OutputDirectory, true);
	FCsvProfiler::Get()->EnableCategoryByString(TEXT("SettlementTrees"));
	FCsvProfiler::Get()->EnableCategoryByString(TEXT("ElementSandboxWorldStorage"));
	FCsvProfiler::Get()->BeginCapture(-1, OutputDirectory, RunLabel + TEXT(".csv"));
	Phase = ERunPhase::Sampling;
	PhaseStartSeconds = FPlatformTime::Seconds();
	if (bLoadPhaseOnly)
	{
		UE_LOG(LogSettlementTreePerformance,
			Display,
			TEXT("%s：开始 %.1f 秒固定观察点装填采样。"),
			*RunLabel,
			SampleDurationSeconds);
	}
	else
	{
		UE_LOG(LogSettlementTreePerformance,
			Display,
			TEXT("%s：开始 %.1f 秒确定性采样路线。"),
			*RunLabel,
			SampleDurationSeconds);
	}
}

void USettlementTreePerformanceWorldSubsystem::BeginIdleSampling()
{
	APlayerController* Controller = GetWorldRef().GetFirstPlayerController();
	const USettlementTreePresentationWorldSubsystem* Trees =
		GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	const USettlementTreeCollisionWorldSubsystem* Collision =
		GetWorldRef().GetSubsystem<USettlementTreeCollisionWorldSubsystem>();
	if (!Controller || !Controller->GetPawn() || !Trees || !Collision)
	{
		return;
	}
	IdleBaseRotation = Controller->GetControlRotation();
	if (UPawnMovementComponent* Movement = Controller->GetPawn()->GetMovementComponent())
	{
		Movement->StopMovementImmediately();
		bPausedMovementForIdle = Movement->IsActive();
		if (bPausedMovementForIdle)
		{
			Movement->Deactivate();
		}
	}
	const FSettlementTreePresentationStats TreeStats = Trees->GetStats();
	const FSettlementTreeCollisionStats CollisionStats = Collision->GetStats();
	IdleBaselineLocalSelectionPassCount = TreeStats.LocalSelectionPassCount;
	IdleBaselineFarSelectionPassCount = TreeStats.FarSelectionPassCount;
	IdleBaselineWorkerDispatchCount = TreeStats.WorkerDispatchCount;
	IdleBaselineCandidateTestCount = TreeStats.CandidateTestCount;
	IdleBaselineCellDeltaEvaluationCount = TreeStats.CellDeltaEvaluationCount;
	IdleBaselineHISMAddCount = TreeStats.HISMAddCount;
	IdleBaselineHISMRemoveCount = TreeStats.HISMRemoveCount;
	IdleBaselineTreeBuildCount = TreeStats.TreeBuildCount;
	IdleBaselineCollisionSourceSubmitCount = CollisionStats.SourceSubmitCount;
	IdleBaselineCollisionQueryCount = CollisionStats.CatalogQueryCount;
	IdleBaselineCollisionCandidateTestCount = CollisionStats.CandidateTestCount;
	IdleFrameMilliseconds.Reset();
	IdleTreeMaintenanceMilliseconds.Reset();
	const int32 ReserveFrames = FMath::CeilToInt(IdleSampleDurationSeconds * 120.0);
	IdleFrameMilliseconds.Reserve(ReserveFrames);
	IdleTreeMaintenanceMilliseconds.Reserve(ReserveFrames);
	Phase = ERunPhase::IdleSampling;
	PhaseStartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogSettlementTreePerformance, Display, TEXT("%s：开始 %.1f 秒固定 Pawn/镜头 Idle 采样。"), *RunLabel,
		   IdleSampleDurationSeconds);
}

void USettlementTreePerformanceWorldSubsystem::CaptureIdleSample(const float DeltaTime)
{
	IdleFrameMilliseconds.Add(FMath::Max(0.0, static_cast<double>(DeltaTime) * 1000.0));
	if (const USettlementTreePresentationWorldSubsystem* Trees =
			GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>())
	{
		IdleTreeMaintenanceMilliseconds.Add(Trees->GetStats().LastObservationMilliseconds);
	}
}

void USettlementTreePerformanceWorldSubsystem::EndIdleSampling()
{
	const USettlementTreePresentationWorldSubsystem* Trees =
		GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	const USettlementTreeCollisionWorldSubsystem* Collision =
		GetWorldRef().GetSubsystem<USettlementTreeCollisionWorldSubsystem>();
	if (!Trees || !Collision)
	{
		return;
	}
	if (bPausedMovementForIdle)
	{
		if (APlayerController* Controller = GetWorldRef().GetFirstPlayerController())
		{
			if (APawn* Pawn = Controller->GetPawn())
			{
				if (UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
				{
					Movement->Activate(true);
				}
			}
		}
		bPausedMovementForIdle = false;
	}
	const FSettlementTreePresentationStats TreeStats = Trees->GetStats();
	const FSettlementTreeCollisionStats CollisionStats = Collision->GetStats();
	IdleLocalSelectionDelta = TreeStats.LocalSelectionPassCount - IdleBaselineLocalSelectionPassCount;
	IdleFarSelectionDelta = TreeStats.FarSelectionPassCount - IdleBaselineFarSelectionPassCount;
	IdleWorkerDispatchDelta = TreeStats.WorkerDispatchCount - IdleBaselineWorkerDispatchCount;
	IdleCandidateTestDelta = TreeStats.CandidateTestCount - IdleBaselineCandidateTestCount;
	IdleCellDeltaEvaluationDelta = TreeStats.CellDeltaEvaluationCount - IdleBaselineCellDeltaEvaluationCount;
	IdleHISMAddDelta = TreeStats.HISMAddCount - IdleBaselineHISMAddCount;
	IdleHISMRemoveDelta = TreeStats.HISMRemoveCount - IdleBaselineHISMRemoveCount;
	IdleTreeBuildDelta = TreeStats.TreeBuildCount - IdleBaselineTreeBuildCount;
	IdleCollisionSourceSubmitDelta = CollisionStats.SourceSubmitCount - IdleBaselineCollisionSourceSubmitCount;
	IdleCollisionQueryDelta = CollisionStats.CatalogQueryCount - IdleBaselineCollisionQueryCount;
	IdleCollisionCandidateTestDelta = CollisionStats.CandidateTestCount - IdleBaselineCollisionCandidateTestCount;
	IdleFrameP99Milliseconds = Percentile99(IdleFrameMilliseconds);
	IdleTreeMaintenanceP99Milliseconds = Percentile99(IdleTreeMaintenanceMilliseconds);
	UE_LOG(LogSettlementTreePerformance, Display,
		   TEXT("%s：Idle 完成，Frame p99 %.3f ms，Tree maintenance p99 %.3f ms，")
			   TEXT("Selection L/F %lld/%lld，Worker %lld，Candidate %lld，Collision Query %lld。"),
		   *RunLabel, IdleFrameP99Milliseconds, IdleTreeMaintenanceP99Milliseconds, IdleLocalSelectionDelta,
		   IdleFarSelectionDelta, IdleWorkerDispatchDelta, IdleCandidateTestDelta, IdleCollisionQueryDelta);
	BeginSampling();
}

void USettlementTreePerformanceWorldSubsystem::DriveRoute(const double SampleSeconds, const float DeltaTime) const
{
	APlayerController* Controller = GetWorldRef().GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Controller || !Pawn)
	{
		return;
	}
	FRotator Target = SampleBaseRotation;
	if (SampleSeconds < 15.0)
	{
		Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.0f);
	}
	else if (SampleSeconds < 30.0)
	{
		Target.Yaw += static_cast<float>((SampleSeconds - 15.0) * 30.0);
		Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 0.65f);
	}
	else if (SampleSeconds < 30.5)
	{
		Target.Yaw += 450.0f + static_cast<float>((SampleSeconds - 30.0) * 360.0);
	}
	else if (SampleSeconds < 32.0)
	{
		Target.Yaw += 630.0f;
	}
	else if (SampleSeconds < 47.0)
	{
		Target.Yaw += 630.0f + 45.0f * FMath::Sin(static_cast<float>((SampleSeconds - 32.0) * UE_TWO_PI * 0.75));
	}
	else
	{
		Target.Yaw += 630.0f;
		Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 0.65f);
	}
	Controller->SetControlRotation(Target);
	(void)DeltaTime;
}

void USettlementTreePerformanceWorldSubsystem::CaptureSample(const float DeltaTime)
{
	FrameMilliseconds.Add(FMath::Max(0.0, static_cast<double>(DeltaTime) * 1000.0));
	if (const USettlementTreePresentationWorldSubsystem* Trees =
			GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>())
	{
		const FSettlementTreePresentationStats Stats = Trees->GetStats();
		if (Stats.WorkerDispatchCount != LastCapturedWorkerDispatchCount)
		{
			SelectionMilliseconds.Add(Stats.LastSelectionMilliseconds);
			LastCapturedWorkerDispatchCount = Stats.WorkerDispatchCount;
		}
		const int64 HISMChanges = Stats.HISMAddCount + Stats.HISMRemoveCount;
		if (HISMChanges != LastCapturedHISMChangeCount)
		{
			ApplyMilliseconds.Add(Stats.LastApplyMilliseconds);
			LastCapturedHISMChangeCount = HISMChanges;
		}
		MaximumHISMCells = FMath::Max(MaximumHISMCells, Stats.HISMCellCount);
		MaximumTreeInstances = FMath::Max(MaximumTreeInstances, Stats.InstanceCount);
	}
	if (const USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		CatalogPublishMilliseconds.Add(Catalog->GetStats().LastPublishMilliseconds);
	}
	if (const UWorldStorageSubsystem* Storage = GetWorldRef().GetSubsystem<UWorldStorageSubsystem>())
	{
		ClientInjectionMilliseconds.Add(Storage->GetRuntimeStats().LastInjectionMilliseconds);
	}
	if (const APlayerController* Controller = GetWorldRef().GetFirstPlayerController())
	{
		if (const UWorldChunkStreamingComponent* Streaming =
				Controller->FindComponentByClass<UWorldChunkStreamingComponent>())
		{
			AuthorityStepMilliseconds.Add(Streaming->GetStreamingStats().AuthorityLastStepMilliseconds);
		}
	}
	MaximumDrawCalls = FMath::Max(MaximumDrawCalls, GNumDrawCallsRHI[0]);
}

void USettlementTreePerformanceWorldSubsystem::EndSampling()
{
	Phase = ERunPhase::Finishing;
	CsvWriteFuture = FCsvProfiler::Get()->EndCapture();
	UE_LOG(LogSettlementTreePerformance, Display, TEXT("%s：采样完成，等待 CSV 写盘。"), *RunLabel);
}

void USettlementTreePerformanceWorldSubsystem::WriteReportAndExit(const FString& CsvPath)
{
	const double FrameP99 = Percentile99(FrameMilliseconds);
	const double SelectionP99 = Percentile99(SelectionMilliseconds);
	const double ApplyP99 = Percentile99(ApplyMilliseconds);
	const double CatalogPublishP99 = Percentile99(CatalogPublishMilliseconds);
	const double ClientInjectionP99 = Percentile99(ClientInjectionMilliseconds);
	const double AuthorityStepP99 = Percentile99(AuthorityStepMilliseconds);
	int64 InvalidVisibleRemovalCount = 0;
	FSettlementTreePresentationStats FinalTreeStats;
	FSettlementTreeCatalogStats FinalCatalogStats;
	if (const USettlementTreePresentationWorldSubsystem* Trees =
			GetWorldRef().GetSubsystem<USettlementTreePresentationWorldSubsystem>())
	{
		FinalTreeStats = Trees->GetStats();
		InvalidVisibleRemovalCount = FinalTreeStats.InvalidVisibleRemovalCount;
	}
	if (const USettlementTreeWorldSubsystem* Catalog = GetWorldRef().GetSubsystem<USettlementTreeWorldSubsystem>())
	{
		FinalCatalogStats = Catalog->GetStats();
	}
	const bool bFramePass = FrameP99 <= 16.67;
	const bool bSelectionPass = SelectionP99 <= 0.5;
	const bool bApplyPass = ApplyP99 <= 3.5;
	const bool bDrawPass = MaximumDrawCalls <= 512;
	const bool bClientInjectionPass = ClientInjectionP99 <= 4.0;
	const bool bAuthorityStepPass = AuthorityStepP99 <= 2.0;
	const bool bActivationPass = ActivationCoreSeconds <= 2.0;
	const bool bVisibleRemovalPass = InvalidVisibleRemovalCount == 0;
	const bool bIdleWorkPass = IdleLocalSelectionDelta == 0 && IdleFarSelectionDelta == 0 &&
							   IdleWorkerDispatchDelta == 0 && IdleCandidateTestDelta == 0 &&
								   IdleCellDeltaEvaluationDelta == 0 && IdleHISMAddDelta == 0 && IdleHISMRemoveDelta == 0 &&
								   IdleTreeBuildDelta == 0 && IdleCollisionSourceSubmitDelta == 0 &&
							   IdleCollisionQueryDelta == 0 && IdleCollisionCandidateTestDelta == 0;
	const bool bIdleMaintenancePass = IdleTreeMaintenanceP99Milliseconds <= 0.10;
	const bool bIdleFramePass = IdleFrameP99Milliseconds <= 16.67;
	const FString Report = FString::Printf(
		TEXT("{\n") TEXT("  \"run\": \"%s\",\n") TEXT("  \"mode\": \"%s\",\n") TEXT("  \"csv\": \"%s\",\n")
			TEXT("  \"sampleFrames\": %d,\n") TEXT("  \"activationCoreSeconds\": %.6f,\n")
				TEXT("  \"frameP99Milliseconds\": %.6f,\n")
				TEXT("  \"treeSelectionGameThreadP99Milliseconds\": %.6f,\n")
				TEXT("  \"treeInstanceApplyP99Milliseconds\": %.6f,\n")
				TEXT("  \"treeCatalogPublishP99Milliseconds\": %.6f,\n")
				TEXT("  \"clientInjectionP99Milliseconds\": %.6f,\n")
				TEXT("  \"authorityStepP99Milliseconds\": %.6f,\n")
				TEXT("  \"maximumDrawCalls\": %d,\n") TEXT("  \"maximumHISMCells\": %d,\n")
				TEXT("  \"maximumTreeInstances\": %d,\n") TEXT("  \"treeCatalogResidentAtEnd\": %d,\n")
				TEXT("  \"treeCatalogCellPublishes\": %lld,\n")
				TEXT("  \"treeSnapshotShardPublishes\": %lld,\n")
				TEXT("  \"treeSnapshotCandidateCopies\": %lld,\n")
				TEXT("  \"treeCandidateTestsAtEnd\": %lld,\n")
				TEXT("  \"treeCellDeltaEvaluationsAtEnd\": %lld,\n")
				TEXT("  \"treeHISMAddsAtEnd\": %lld,\n") TEXT("  \"treeHISMRemovesAtEnd\": %lld,\n")
				TEXT("  \"treeBuildsAtEnd\": %lld,\n") TEXT("  \"treeBuildsInFlightAtEnd\": %d,\n")
				TEXT("  \"maximumConcurrentTreeBuildsObserved\": %d,\n")
				TEXT("  \"invalidVisibleRemovalCount\": %lld,\n") TEXT("  \"idle\": {\n")
				TEXT("    \"frameP99Milliseconds\": %.6f, \"treeMaintenanceP99Milliseconds\": %.6f,\n")
				TEXT("    \"localSelectionDelta\": %lld, \"farSelectionDelta\": %lld, ")
				TEXT("\"workerDispatchDelta\": %lld, \"candidateTestDelta\": %lld,\n")
				TEXT("    \"cellDeltaEvaluationDelta\": %lld, \"hismAddDelta\": %lld, ")
				TEXT("\"hismRemoveDelta\": %lld, \"treeBuildDelta\": %lld,\n")
				TEXT("    \"collisionSourceSubmitDelta\": %lld, \"collisionQueryDelta\": %lld, ")
				TEXT("\"collisionCandidateTestDelta\": %lld\n") TEXT("  },\n")
				TEXT("  \"passes\": {\n")
				TEXT("    \"frame\": %s, \"selection\": %s, \"apply\": %s, \"drawCalls\": %s,\n")
				TEXT("    \"clientInjection\": %s, \"authorityStep\": %s, \"activationCore\": %s, ")
				TEXT("\"visibleRemoval\": %s,\n")
				TEXT("    \"idleZeroWork\": %s, \"idleMaintenance\": %s, \"idleFrame\": %s\n")
				TEXT("  }\n") TEXT("}\n"),
		*RunLabel, bLoadPhaseOnly ? TEXT("load") : (bRouteDuringLoad ? TEXT("route-load") : TEXT("route")),
		*CsvPath.ReplaceCharWithEscapedChar(), FrameMilliseconds.Num(), ActivationCoreSeconds, FrameP99, SelectionP99,
		ApplyP99, CatalogPublishP99, ClientInjectionP99, AuthorityStepP99, MaximumDrawCalls, MaximumHISMCells,
		MaximumTreeInstances, FinalCatalogStats.ResidentTreeCount, FinalCatalogStats.CellPublishCount,
		FinalCatalogStats.SnapshotShardPublishCount, FinalCatalogStats.SnapshotCandidateCopyCount,
		FinalTreeStats.CandidateTestCount, FinalTreeStats.CellDeltaEvaluationCount, FinalTreeStats.HISMAddCount,
		FinalTreeStats.HISMRemoveCount, FinalTreeStats.TreeBuildCount, FinalTreeStats.InFlightTreeBuildCount,
		FinalTreeStats.MaximumConcurrentTreeBuildsObserved, InvalidVisibleRemovalCount, IdleFrameP99Milliseconds,
		IdleTreeMaintenanceP99Milliseconds, IdleLocalSelectionDelta, IdleFarSelectionDelta, IdleWorkerDispatchDelta,
		IdleCandidateTestDelta, IdleCellDeltaEvaluationDelta, IdleHISMAddDelta, IdleHISMRemoveDelta, IdleTreeBuildDelta,
		IdleCollisionSourceSubmitDelta, IdleCollisionQueryDelta, IdleCollisionCandidateTestDelta, *JsonBool(bFramePass),
		*JsonBool(bSelectionPass), *JsonBool(bApplyPass), *JsonBool(bDrawPass), *JsonBool(bClientInjectionPass),
		*JsonBool(bAuthorityStepPass), *JsonBool(bActivationPass), *JsonBool(bVisibleRemovalPass),
		*JsonBool(bIdleWorkPass), *JsonBool(bIdleMaintenancePass), *JsonBool(bIdleFramePass));
	const FString ReportPath = FPaths::Combine(OutputDirectory, RunLabel + TEXT(".json"));
	const bool bSaved =
		FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogSettlementTreePerformance, Display,
		   TEXT("%s：报告%s %s；Frame p99 %.3f ms，Selection p99 %.3f ms，Apply p99 %.3f ms，Draw %d。"), *RunLabel,
		   bSaved ? TEXT("已写入") : TEXT("写入失败"), *ReportPath, FrameP99, SelectionP99, ApplyP99, MaximumDrawCalls);
	Phase = ERunPhase::Complete;
	FPlatformMisc::RequestExit(false);
}
