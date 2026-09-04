#pragma once
#include "CoreMinimal.h"
#include "MeteorClientRuntime.h"
#include "MeteorWorldSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "MeteorClientWorldSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FMeteorClientPresentationChangesEvent,
	TConstArrayView<UE::ElementSandbox::Meteor::FMeteorClientPresentationLane>);
DECLARE_MULTICAST_DELEGATE_OneParam(FMeteorClientBurstRetiredEvent, UE::ElementSandbox::Meteor::FMeteorBurstId);

/** 客户端协议状态与事件发布；不拥有渲染资源，也不等待渲染完成来确认 Settlement。 */
UCLASS()
class ELEMENTSANDBOXMETEOR_API UMeteorClientWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	void QueuePreparedTrajectoryPage(TSharedPtr<const UE::ElementSandbox::Meteor::FMeteorTrajectoryPage> Page);
	void QueueTrajectoryActivation(const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation& Activation);
	void QueueTrajectoryCancellation(const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation& Cancellation);
	void QueueTrajectoryPageCancellation(UE::ElementSandbox::Meteor::FMeteorBurstId BurstId, uint64 PageId, uint32 Revision);
	void QueueSettlementMappings(TConstArrayView<UE::ElementSandbox::Meteor::FMeteorSettlementMapping> Mappings);
	const UE::ElementSandbox::Meteor::FMeteorClientRuntime& GetRuntime() const { return ClientRuntime; }
	const UE::ElementSandbox::Meteor::FMeteorRuntimeConfig& GetRuntimeConfig() const { return RuntimeConfig; }
	FMeteorClientPresentationChangesEvent& OnPresentationChanges() { return PresentationChangesEvent; }
	FMeteorClientBurstRetiredEvent& OnBurstRetired() { return BurstRetiredEvent; }
protected:
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
	void HandleLocalTrajectoryPagePrepared(const UE::ElementSandbox::Meteor::FMeteorTrajectoryPage& Page);
	void AdoptBurst(UE::ElementSandbox::Meteor::FMeteorBurstId BurstId);
	void PublishChanges(double TimeOffset);
	struct FActivationWork { UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation Command; int32 Cursor = 0; };
	struct FPageCancellation { UE::ElementSandbox::Meteor::FMeteorBurstId BurstId; uint64 PageId = 0; uint32 Revision = 0; };
	UE::ElementSandbox::Meteor::FMeteorClientRuntime ClientRuntime;
	UE::ElementSandbox::Meteor::FMeteorRuntimeConfig RuntimeConfig;
	UE::ElementSandbox::Meteor::FMeteorBurstId ActiveBurst;
	FMeteorClientPresentationChangesEvent PresentationChangesEvent;
	FMeteorClientBurstRetiredEvent BurstRetiredEvent;
	TArray<TSharedPtr<const UE::ElementSandbox::Meteor::FMeteorTrajectoryPage>> Prepared;
	TArray<FActivationWork> Activations;
	TArray<UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation> Cancellations;
	TArray<FPageCancellation> PageCancellations;
	TArray<UE::ElementSandbox::Meteor::FMeteorSettlementMapping> Settlements;
	FDelegateHandle LocalPagePreparedHandle, LocalPageActivatedHandle, LocalPageCanceledHandle, LocalSettlementHandle;
};
