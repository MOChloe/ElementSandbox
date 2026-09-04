#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "MeteorPageScheduler.h"
#include "MeteorRuntimeTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "MeteorWorldSubsystem.generated.h"

namespace UE::ElementSandbox::Meteor
{
	class FMeteorProductSink;
	struct FMeteorWorldRuntime;

	struct ELEMENTSANDBOXMETEOR_API FMeteorSettlementMapping final
	{
		FMeteorDebrisKey Debris;
		FWorldEntityId WorldEntityId;
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorAuthorityStats final
	{
		bool bBurstActive = false;
		FMeteorBurstId BurstId;
		double PublishedWaveRadius = 0.0;
		int32 QueriedTiles = 0;
		int32 TotalTiles = 0;
		int32 PendingTargets = 0;
		int32 CoreCandidateCount = 0;
		int32 ImpactFrameDestroyedTargets = 0;
		int32 FirstActivationLaneCount = 0;
		uint32 TotalActivatedLaneCount = 0;
		double ImpactToFirstActivationMilliseconds = -1.0;
		int32 WorkerInFlight = 0;
		int32 SettlementBacklog = 0;
		double LastPumpMilliseconds = 0.0;
		FMeteorSchedulerStats Scheduler;
	};
}

DECLARE_MULTICAST_DELEGATE_OneParam(
	FMeteorTrajectoryPagePreparedEvent,
	const UE::ElementSandbox::Meteor::FMeteorTrajectoryPage&);
DECLARE_MULTICAST_DELEGATE_OneParam(
	FMeteorTrajectoryActivatedEvent,
	const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation&);
DECLARE_MULTICAST_DELEGATE_OneParam(
	FMeteorTrajectoryCanceledEvent,
	const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation&);
DECLARE_MULTICAST_DELEGATE_OneParam(
	FMeteorSettlementPublishedEvent,
	TConstArrayView<UE::ElementSandbox::Meteor::FMeteorSettlementMapping>);

/** 独立于 Element 8 Hz 与 Chaos 的陨石 Authority/解析碎片 World Runtime。 */
UCLASS()
class ELEMENTSANDBOXMETEOR_API UMeteorWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UMeteorWorldSubsystem();
	virtual ~UMeteorWorldSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

	/** Authority 排程唯一 Burst；ImpactTime 前只做轻量预演，不提前提交源销毁。 */
	bool ScheduleStrike(
		const FVector& ImpactCenter,
		double ImpactTimeSeconds,
		UE::ElementSandbox::Meteor::FMeteorBurstId& OutBurstId);
	/** 只允许撤销尚未到达 ImpactTime、且没有发布任何源破坏的排程。 */
	bool CancelScheduledStrike(UE::ElementSandbox::Meteor::FMeteorBurstId BurstId);
	bool HasActiveBurst() const;
	bool HasRuntimeState() const { return Runtime.IsValid(); }
	/** Authority 只在角色前方近距离的 Resident Chunk 中选择有宿主的展示落点；不改变 Residency。 */
	bool TryGetMapImpactLocation(
		const FVector& ViewerLocation,
		const FVector& ViewerForward,
		FVector& OutImpactLocation) const;
	UE::ElementSandbox::Meteor::FMeteorBurstId GetActiveBurstId() const;
	UE::ElementSandbox::Meteor::FMeteorRuntimeConfig GetRuntimeConfig() const;
	UE::ElementSandbox::Meteor::FMeteorAuthorityStats GetAuthorityStats() const;
	/** 新连接先取仍有激活 Lane 的不可变 Payload，再取轻量 Activation 历史。 */
	void GetPreparedTrajectoryPages(
		TArray<UE::ElementSandbox::Meteor::FMeteorTrajectoryPage>& OutPages) const;
	void GetPublishedTrajectoryActivations(
		TArray<UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation>& OutActivations) const;
	void GetPublishedSettlementMappings(
		TArray<UE::ElementSandbox::Meteor::FMeteorSettlementMapping>& OutMappings) const;

	FMeteorTrajectoryPagePreparedEvent& OnTrajectoryPagePrepared();
	FMeteorTrajectoryActivatedEvent& OnTrajectoryActivated();
	FMeteorTrajectoryCanceledEvent& OnTrajectoryCanceled();
	FMeteorSettlementPublishedEvent& OnSettlementPublished();

	#if WITH_DEV_AUTOMATION_TESTS
	void OverrideRuntimeConfigForTesting(
		const UE::ElementSandbox::Meteor::FMeteorRuntimeConfig& Config);
	#endif

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	/** Authority 的到期事务先于新预演推进；单批上限只约束不可抢占的 Stage/Commit。 */
	void PumpSettlements(double NowSeconds, double DeadlineSeconds);

	TPimplPtr<UE::ElementSandbox::Meteor::FMeteorWorldRuntime> Runtime;
	friend UE::ElementSandbox::Meteor::FMeteorProductSink;
};
