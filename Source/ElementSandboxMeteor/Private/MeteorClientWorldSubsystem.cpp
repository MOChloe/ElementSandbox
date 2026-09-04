#include "MeteorClientWorldSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CsvProfiler.h"

using namespace UE::ElementSandbox::Meteor;
CSV_DEFINE_CATEGORY(MeteorClient, true);

void UMeteorClientWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UMeteorWorldSubsystem>();
	UMeteorWorldSubsystem* Authority = GetWorld()->GetSubsystem<UMeteorWorldSubsystem>();
	RuntimeConfig = Authority ? Authority->GetRuntimeConfig() : FMeteorRuntimeConfig{};
	ClientRuntime.Initialize(RuntimeConfig);
	if (Authority && !GetWorld()->IsNetMode(NM_Client) && !GetWorld()->IsNetMode(NM_DedicatedServer))
	{
		LocalPagePreparedHandle = Authority->OnTrajectoryPagePrepared().AddUObject(this, &ThisClass::HandleLocalTrajectoryPagePrepared);
		LocalPageActivatedHandle = Authority->OnTrajectoryActivated().AddUObject(this, &ThisClass::QueueTrajectoryActivation);
		LocalPageCanceledHandle = Authority->OnTrajectoryCanceled().AddUObject(this, &ThisClass::QueueTrajectoryCancellation);
		LocalSettlementHandle = Authority->OnSettlementPublished().AddUObject(this, &ThisClass::QueueSettlementMappings);
	}
}
void UMeteorClientWorldSubsystem::Deinitialize()
{
	if (auto* Authority = GetWorld()->GetSubsystem<UMeteorWorldSubsystem>(); Authority && Authority->HasRuntimeState())
	{
		Authority->OnTrajectoryPagePrepared().Remove(LocalPagePreparedHandle);
		Authority->OnTrajectoryActivated().Remove(LocalPageActivatedHandle);
		Authority->OnTrajectoryCanceled().Remove(LocalPageCanceledHandle);
		Authority->OnSettlementPublished().Remove(LocalSettlementHandle);
	}
	ClientRuntime.Reset(); Prepared.Reset(); Activations.Reset(); Cancellations.Reset(); PageCancellations.Reset(); Settlements.Reset();
	Super::Deinitialize();
}
void UMeteorClientWorldSubsystem::HandleLocalTrajectoryPagePrepared(const FMeteorTrajectoryPage& Page)
{
	QueuePreparedTrajectoryPage(MakeShared<FMeteorTrajectoryPage>(Page));
}
void UMeteorClientWorldSubsystem::QueuePreparedTrajectoryPage(TSharedPtr<const FMeteorTrajectoryPage> Page)
{
	if (Page && Page->IsValid()) { AdoptBurst(Page->BurstId); if (Page->BurstId == ActiveBurst) Prepared.Add(MoveTemp(Page)); }
}
void UMeteorClientWorldSubsystem::QueueTrajectoryActivation(const FMeteorTrajectoryActivation& Item)
{
	if (Item.IsValid()) { AdoptBurst(Item.BurstId); if (Item.BurstId == ActiveBurst) Activations.Add({Item, 0}); }
}
void UMeteorClientWorldSubsystem::QueueTrajectoryCancellation(const FMeteorTrajectoryActivation& Item)
{
	if (Item.IsCancellationValid()) { AdoptBurst(Item.BurstId); if (Item.BurstId == ActiveBurst) Cancellations.Add(Item); }
}
void UMeteorClientWorldSubsystem::QueueTrajectoryPageCancellation(FMeteorBurstId Burst, uint64 Page, uint32 Revision)
{
	if (Burst.IsSet() && Page && Revision) { AdoptBurst(Burst); if (Burst == ActiveBurst) PageCancellations.Add({Burst, Page, Revision}); }
}
void UMeteorClientWorldSubsystem::QueueSettlementMappings(TConstArrayView<FMeteorSettlementMapping> Items)
{
	for (const auto& Item : Items)
		if (Item.Debris.IsSet() && Item.WorldEntityId.IsSet())
		{
			AdoptBurst(Item.Debris.BurstId);
			if (Item.Debris.BurstId == ActiveBurst) Settlements.Add(Item);
		}
}
void UMeteorClientWorldSubsystem::AdoptBurst(FMeteorBurstId Burst)
{
	if (!Burst.IsSet() || Burst.Value <= ActiveBurst.Value) return;
	if (ActiveBurst.IsSet()) BurstRetiredEvent.Broadcast(ActiveBurst);
	ClientRuntime.Initialize(RuntimeConfig); ActiveBurst = Burst;
	Prepared.Reset(); Activations.Reset(); Cancellations.Reset(); PageCancellations.Reset(); Settlements.Reset();
}
void UMeteorClientWorldSubsystem::PublishChanges(double Offset)
{
	if (!ClientRuntime.HasPendingPresentationChanges()) return;
	TArray<FMeteorClientPresentationLane> Changes;
	ClientRuntime.ConsumePresentationChanges(Changes, Offset);
	for (auto& Change : Changes) Change.BurstId = ActiveBurst;
	PresentationChangesEvent.Broadcast(Changes);
}
void UMeteorClientWorldSubsystem::Tick(float DeltaTime)
{
	CSV_SCOPED_TIMING_STAT(MeteorClient, Tick);
	const double Start = FPlatformTime::Seconds();
	const double Deadline = Start + RuntimeConfig.ClientGameThreadBudgetMilliseconds / 1000.0;
	double ServerNow = GetWorld()->GetTimeSeconds();
	if (GetWorld()->IsNetMode(NM_Client))
		if (auto* GameState = GetWorld()->GetGameState()) ServerNow = GameState->GetServerWorldTimeSeconds();
	const double Offset = GetWorld()->GetTimeSeconds() - ServerNow;
	// 控制消息先写终态；已确认的落地物件不能被迟到的 Payload/Activate 重新激活。
	for (const auto& Item : PageCancellations) ClientRuntime.CancelTrajectoryPage(Item.PageId, Item.Revision);
	PageCancellations.Reset();
	for (const auto& Item : Cancellations) ClientRuntime.CancelTrajectoryLanes(Item.PageId, Item.Revision, Item.Ordinals);
	Cancellations.Reset();
	for (const auto& Item : Settlements) ClientRuntime.MarkSettled(Item.Debris.DebrisOrdinal, Item.WorldEntityId);
	Settlements.Reset();
	int32 Consumed = 0;
	while (Consumed < Prepared.Num() && FPlatformTime::Seconds() < Deadline)
	{
		ClientRuntime.PrepareTrajectoryPage(Prepared[Consumed++]);
	}
	Prepared.RemoveAt(0, Consumed, EAllowShrinking::No);
	PublishChanges(Offset);
	const double PrepareEnd = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Activations.Num() && FPlatformTime::Seconds() < Deadline;)
	{
		auto& Work = Activations[Index];
		const uint32 Revision = ClientRuntime.GetPreparedPageRevision(Work.Command.PageId);
		if (!Revision) { ++Index; continue; }
		if (Revision == Work.Command.Revision)
			ClientRuntime.ActivateTrajectoryLanes(Work.Command.PageId, Revision, Work.Command.Ordinals, Work.Command.AuthorityStartTimeSeconds);
		Activations.RemoveAt(Index, 1, EAllowShrinking::No);
	}
	PublishChanges(Offset);
	ClientRuntime.SetPipelineStats(Prepared.Num(), Activations.Num(), (PrepareEnd - Start) * 1000.0,
		(FPlatformTime::Seconds() - PrepareEnd) * 1000.0, FPlatformTime::Seconds() >= Deadline);
	CSV_CUSTOM_STAT(MeteorClient, PreparedPages, ClientRuntime.GetStats().PreparedPageCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(MeteorClient, FlyingLanes, ClientRuntime.GetStats().FlyingLaneCount, ECsvCustomStatOp::Set);
}
bool UMeteorClientWorldSubsystem::IsTickable() const
{
	return GetWorld() && !GetWorld()->IsNetMode(NM_DedicatedServer)
		&& (!Prepared.IsEmpty() || !Activations.IsEmpty() || !Cancellations.IsEmpty() || !PageCancellations.IsEmpty() || !Settlements.IsEmpty());
}
TStatId UMeteorClientWorldSubsystem::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UMeteorClientWorldSubsystem, STATGROUP_Tickables); }
bool UMeteorClientWorldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const { return Type == EWorldType::Game || Type == EWorldType::PIE; }
