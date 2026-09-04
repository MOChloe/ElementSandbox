#pragma once

#include "Async/Future.h"
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SettlementTreePerformanceWorldSubsystem.generated.h"

/**
 * 仅在 -SettlementTreePerfRun=Label 时启用的 Client A/B 夹具。
 * 默认执行稳定 Idle + 路线；-SettlementTreePerfLoadOnly 固定观察点采集装填阶段；
 * -SettlementTreePerfRouteDuringLoad 从 Activation Core 起在装填阶段直接执行路线。
 */
UCLASS()
class USettlementTreePerformanceWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	enum class ERunPhase : uint8
	{
		WaitingForClient,
		Warmup,
		WaitingForIdleStable,
		IdleSampling,
		Sampling,
		Finishing,
		Complete
	};

	void BeginSampling();
	void BeginIdleSampling();
	void CaptureIdleSample(float DeltaTime);
	void EndIdleSampling();
	void DriveRoute(double SampleSeconds, float DeltaTime) const;
	void CaptureSample(float DeltaTime);
	void EndSampling();
	void WriteReportAndExit(const FString& CsvPath);

	bool bEnabled = false;
	bool bLoadPhaseOnly = false;
	bool bRouteDuringLoad = false;
	FString RunLabel;
	FString OutputDirectory;
	double WarmupSeconds = 10.0;
	double SampleDurationSeconds = 60.0;
	double IdleSampleDurationSeconds = 30.0;
	double IdleStableStartSeconds = 0.0;
	double NextIdleWaitDiagnosticSeconds = 0.0;
	double PhaseStartSeconds = 0.0;
	double ActivationCoreSeconds = 0.0;
	ERunPhase Phase = ERunPhase::WaitingForClient;
	FRotator SampleBaseRotation = FRotator::ZeroRotator;
	FRotator IdleBaseRotation = FRotator::ZeroRotator;
	bool bPausedMovementForIdle = false;
	TSharedFuture<FString> CsvWriteFuture;

	TArray<double> FrameMilliseconds;
	TArray<double> SelectionMilliseconds;
	TArray<double> ApplyMilliseconds;
	TArray<double> CatalogPublishMilliseconds;
	TArray<double> ClientInjectionMilliseconds;
	TArray<double> AuthorityStepMilliseconds;
	TArray<double> IdleFrameMilliseconds;
	TArray<double> IdleTreeMaintenanceMilliseconds;
	int32 MaximumDrawCalls = 0;
	int32 MaximumHISMCells = 0;
	int32 MaximumTreeInstances = 0;
	int64 LastCapturedWorkerDispatchCount = 0;
	int64 LastCapturedHISMChangeCount = 0;
	int64 IdleBaselineLocalSelectionPassCount = 0;
	int64 IdleBaselineFarSelectionPassCount = 0;
	int64 IdleBaselineWorkerDispatchCount = 0;
	int64 IdleBaselineCandidateTestCount = 0;
	int64 IdleBaselineCellDeltaEvaluationCount = 0;
	int64 IdleBaselineHISMAddCount = 0;
	int64 IdleBaselineHISMRemoveCount = 0;
	int64 IdleBaselineTreeBuildCount = 0;
	int64 IdleBaselineCollisionSourceSubmitCount = 0;
	int64 IdleBaselineCollisionQueryCount = 0;
	int64 IdleBaselineCollisionCandidateTestCount = 0;
	int64 IdleLocalSelectionDelta = 0;
	int64 IdleFarSelectionDelta = 0;
	int64 IdleWorkerDispatchDelta = 0;
	int64 IdleCandidateTestDelta = 0;
	int64 IdleCellDeltaEvaluationDelta = 0;
	int64 IdleHISMAddDelta = 0;
	int64 IdleHISMRemoveDelta = 0;
	int64 IdleTreeBuildDelta = 0;
	int64 IdleCollisionSourceSubmitDelta = 0;
	int64 IdleCollisionQueryDelta = 0;
	int64 IdleCollisionCandidateTestDelta = 0;
	double IdleFrameP99Milliseconds = 0.0;
	double IdleTreeMaintenanceP99Milliseconds = 0.0;
};
