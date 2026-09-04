#pragma once

#include "CoreMinimal.h"
#include "ElementPresentationTypes.h"
#include "ElementVisualDefinition.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Visual/ElementVisualTypes.h"

#include "ElementPresentationWorldSubsystem.generated.h"

class FElementPresentationWorldData;

/** Independent, event-driven Element client projection. */
UCLASS()
class ELEMENTSANDBOXELEMENTPRESENTATION_API UElementPresentationWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UElementPresentationWorldSubsystem();
	virtual ~UElementPresentationWorldSubsystem() override;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

	/** 只能在尚未接入 Source/Definition/实例之前替换，避免运行中全局重建。 */
	bool Configure(const FElementPresentationConfig& Config);
	FElementVisualSourceHandle RegisterVisualSource(
		TSharedRef<IElementVisualSource, ESPMode::ThreadSafe> Source);
	bool UnregisterVisualSource(FElementVisualSourceHandle Handle);
	bool RegisterVisualDefinition(const FElementVisualDefinition& Definition);
	bool UnregisterVisualDefinition(FName DefinitionId);
	bool IsConfigured() const;

	FElementPresentationStats GetStats() const;
	FElementPresentationDebugSnapshot CopyDebugSnapshot() const;
	bool IsPresentationStateAllocated() const { return Data.IsValid(); }

#if WITH_DEV_AUTOMATION_TESTS
	void SetSynchronousBuildsForTesting(bool bSynchronous);
	void SetBuildDispatchHeldForTesting(bool bHeld);
	void ReleaseHeldBuildsForTesting();
	void SetJournalConsumptionPausedForTesting(bool bPaused);
	void SetApplyFailureCountForTesting(int32 FailureCount);
	bool PumpUntilIdleForTesting(int32 MaxIterations = 4096);
	void ExpireAllGraceForTesting();
	bool IsVisualAppliedForTesting(const FElementVisualKey& Key) const;
	uint64 GetAppliedVisualRevisionForTesting(const FElementVisualKey& Key) const;
	int32 GetCoverageRefCountForTesting(FElementVisualShardKey Shard) const;
#endif

private:
	void HandleViewSourceUpdated(const struct FPresentationViewSource& View);
	void HandleViewSourceRemoved(struct FPresentationSourceHandle Source);
	void HandleVisualChangesAvailable(const TArray<FElementVisualShardKey>& Shards);
	void HandleGraceTimer();
	void ScheduleNextGraceTimer();

	TPimplPtr<FElementPresentationWorldData> Data;
};
