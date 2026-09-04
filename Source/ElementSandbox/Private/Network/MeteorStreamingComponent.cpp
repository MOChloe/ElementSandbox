#include "Network/MeteorStreamingComponent.h"

#include "BuildingWorldSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/NetConnection.h"
#include "HAL/PlatformTime.h"
#include "MeteorClientWorldSubsystem.h"
#include "Network/MeteorActivationCausalGate.h"
#include "MeteorRuntimeTypes.h"
#include "MeteorWorldSubsystem.h"
#include "Misc/Crc.h"
#include "NetBulkTransferActorChannel.h"
#include "NetBulkTransferScheduler.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Tree/SettlementTreePresentationWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

using namespace UE::ElementSandbox::Meteor;
using namespace UE::ElementSandbox::NetBulk;

DEFINE_LOG_CATEGORY_STATIC(LogElementSandboxMeteorStream, Log, All);
CSV_DEFINE_CATEGORY(MeteorSourceGate, true);

namespace
{
	constexpr uint32 MeteorTransferDomain = 0x4d455452; // METR
	constexpr int32 MaximumSourceCommitBatchSize = 256;
	constexpr int32 MaximumSourceProjectionChecksPerFrame = 512;
	constexpr double SourceProjectionCheckBudgetSeconds = 0.00025;

	FPayloadId ToPayloadId(const FMeteorNetPageId& Id)
	{
		return {MeteorTransferDomain, Id.PageId ^ (Id.BurstId * 0x9e3779b97f4a7c15ull), Id.Revision};
	}

	template <typename T>
	void CompactConsumedQueue(TArray<T>& Queue, int32& Head)
	{
		// 队列积压时按几何比例回收，避免每消费 1024 条就搬动剩余几十万条记录。
		if (Head > 0 && ((Head >= 1024 && Head >= Queue.Num() / 2) || Head == Queue.Num()))
		{
			Queue.RemoveAt(0, Head, EAllowShrinking::No);
			Head = 0;
		}
	}
}

void FMeteorLaneControlSendWindow::Enqueue(
	FMeteorNetPageActivation Activation,
	const bool bCancellation)
{
	if (!Activation.Id.IsValid() || Activation.Ordinals.IsEmpty())
	{
		return;
	}
	for (int32 Offset = 0; Offset < Activation.Ordinals.Num();
		Offset += MaximumOrdinalsPerRecord)
	{
		FMeteorNetPageActivation Chunk = Activation;
		Chunk.Ordinals.Reset();
		const int32 Count = FMath::Min(
			MaximumOrdinalsPerRecord, Activation.Ordinals.Num() - Offset);
		Chunk.Ordinals.Append(Activation.Ordinals.GetData() + Offset, Count);
		FMeteorNetLaneControl& Control = PendingControls.AddDefaulted_GetRef();
		Control.bCancellation = bCancellation;
		Control.Activation = MoveTemp(Chunk);
	}
}

void FMeteorLaneControlSendWindow::EnqueueSettlements(TConstArrayView<FMeteorNetSettlement> Settlements)
{
	PendingSettlements.Append(Settlements.GetData(), Settlements.Num());
}

int32 FMeteorLaneControlSendWindow::GetNextBatchEstimatedBytes() const
{
	if (InFlightSequences.Num() >= MaximumInFlightBatches) return 0;
	int32 Records = 0, Ordinals = 0;
	for (int32 Index = PendingControlHead; Index < PendingControls.Num() && Records < MaximumRecordsPerBatch; ++Index)
	{
		const int32 Count = PendingControls[Index].Activation.Ordinals.Num();
		if (Records > 0 && Ordinals + Count > MaximumOrdinalsPerBatch) break;
		Ordinals += Count;
		++Records;
	}
	const int32 Settlements = FMath::Min(MaximumSettlementsPerBatch, GetPendingSettlementCount());
	return Records > 0 || Settlements > 0 ? 1024 + Records * 64 + Ordinals * 4 + Settlements * 32 : 0;
}

bool FMeteorLaneControlSendWindow::TryBuildBatch(FMeteorNetLaneControlBatch& OutBatch)
{
	OutBatch = {};
	if (InFlightSequences.Num() >= MaximumInFlightBatches
		|| (PendingControlHead >= PendingControls.Num() && PendingSettlementHead >= PendingSettlements.Num()))
	{
		return false;
	}

	int32 OrdinalCount = 0;
	while (PendingControlHead < PendingControls.Num()
		&& OutBatch.Controls.Num() < MaximumRecordsPerBatch)
	{
		const FMeteorNetLaneControl& Candidate = PendingControls[PendingControlHead];
		const int32 CandidateOrdinalCount = Candidate.Activation.Ordinals.Num();
		if (!OutBatch.Controls.IsEmpty()
			&& OrdinalCount + CandidateOrdinalCount > MaximumOrdinalsPerBatch)
		{
			break;
		}
		OrdinalCount += CandidateOrdinalCount;
		OutBatch.Controls.Add(MoveTemp(PendingControls[PendingControlHead++]));
	}
	const int32 SettlementCount = FMath::Min(MaximumSettlementsPerBatch, GetPendingSettlementCount());
	if (SettlementCount > 0)
	{
		OutBatch.Settlements.Append(PendingSettlements.GetData() + PendingSettlementHead, SettlementCount);
		PendingSettlementHead += SettlementCount;
	}
	if (OutBatch.Controls.IsEmpty() && OutBatch.Settlements.IsEmpty())
	{
		return false;
	}
	OutBatch.Sequence = NextSequence++;
	if (NextSequence == 0)
	{
		NextSequence = 1;
	}
	InFlightSequences.Add(OutBatch.Sequence);
	CompactConsumedQueue(PendingControls, PendingControlHead);
	CompactConsumedQueue(PendingSettlements, PendingSettlementHead);
	return true;
}

bool FMeteorLaneControlSendWindow::Acknowledge(const uint32 Sequence)
{
	return Sequence != 0 && InFlightSequences.Remove(Sequence) > 0;
}

void FMeteorLaneControlSendWindow::Reset()
{
	PendingControls.Reset();
	PendingSettlements.Reset();
	InFlightSequences.Reset();
	PendingControlHead = 0;
	PendingSettlementHead = 0;
	NextSequence = 1;
}

UMeteorStreamingComponent::UMeteorStreamingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.0f;
	SetIsReplicatedByDefault(true);
}

void UMeteorStreamingComponent::SetBulkTransferScheduler(
	TSharedPtr<FConnectionScheduler> InScheduler)
{
	check(!HasBegunPlay());
	ServerScheduler = MoveTemp(InScheduler);
}

void UMeteorStreamingComponent::BeginPlay()
{
	Super::BeginPlay();
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller) return;
	if (Controller->HasAuthority() && !Controller->IsLocalController())
	{
		if (!ServerScheduler)
		{
			ServerScheduler = MakeShared<FConnectionScheduler>();
		}
		BindActorChannelTransport(*ServerScheduler, Controller);
	}
	else if (!Controller->HasAuthority() && Controller->IsLocalController())
	{
		ServerStartMeteorStreaming();
	}
}

void UMeteorStreamingComponent::ServerStartMeteorStreaming_Implementation()
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (bServerStreamingStarted || !Controller || !Controller->HasAuthority() || Controller->IsLocalController()) return;
	UMeteorWorldSubsystem* Meteor = GetWorld()->GetSubsystem<UMeteorWorldSubsystem>();
	if (!Meteor) return;
	bServerStreamingStarted = true;
	// 服务端 Actor BeginPlay 时，晚加入客户端的 Actor Channel 尚未建立，直接发送的 Offer 会丢失。
	// 客户端端点握手后才回放已有页；此后订阅事件，两条路径在 GameThread 上没有遗漏窗口。
	PagePreparedHandle = Meteor->OnTrajectoryPagePrepared().AddUObject(this, &ThisClass::HandlePagePrepared);
	PageActivatedHandle = Meteor->OnTrajectoryActivated().AddUObject(this, &ThisClass::HandlePageActivated);
	PageCanceledHandle = Meteor->OnTrajectoryCanceled().AddUObject(this, &ThisClass::HandlePageCanceled);
	SettlementPublishedHandle = Meteor->OnSettlementPublished().AddUObject(this, &ThisClass::HandleSettlementPublished);
	TArray<FMeteorTrajectoryPage> PreparedPages;
	Meteor->GetPreparedTrajectoryPages(PreparedPages);
	for (const FMeteorTrajectoryPage& Page : PreparedPages) HandlePagePrepared(Page);
	TArray<FMeteorTrajectoryActivation> Activations;
	Meteor->GetPublishedTrajectoryActivations(Activations);
	for (const FMeteorTrajectoryActivation& Activation : Activations) HandlePageActivated(Activation);
	TArray<FMeteorSettlementMapping> ExistingSettlements;
	Meteor->GetPublishedSettlementMappings(ExistingSettlements);
	HandleSettlementPublished(ExistingSettlements);
}

void UMeteorStreamingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ServerScheduler)
	{
		for (const TPair<FMeteorNetPageId, FServerPayloadState>& Pair : ServerPayloads)
		{
			ServerScheduler->Cancel(ToPayloadId(Pair.Key));
		}
	}
	if (UMeteorWorldSubsystem* Meteor = GetWorld() ? GetWorld()->GetSubsystem<UMeteorWorldSubsystem>() : nullptr;
		Meteor && Meteor->HasRuntimeState())
	{
		Meteor->OnTrajectoryPagePrepared().Remove(PagePreparedHandle);
		Meteor->OnTrajectoryActivated().Remove(PageActivatedHandle);
		Meteor->OnTrajectoryCanceled().Remove(PageCanceledHandle);
		Meteor->OnSettlementPublished().Remove(SettlementPublishedHandle);
	}
	ServerScheduler.Reset();
	bServerStreamingStarted = false;
	ServerPayloads.Reset();
	ServerLaneControlWindow.Reset();
	ClientAssemblies.Reset();
	ClientSourceCausalGates.Reset();
	ClientPendingSourceCommits.Reset();
	ClientAwaitingSourceProjection.Reset();
	ClientCanceledPages.Reset();
	ClientPendingSourceCommitHead = 0;
	ClientAwaitingSourceProjectionHead = 0;
	Super::EndPlay(EndPlayReason);
}

void UMeteorStreamingComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !GetWorld()) return;
	if (Controller->HasAuthority() && ServerScheduler)
	{
		FMeteorNetLaneControlBatch LaneControlBatch;
		while (CanSendReliablePayload(Controller, ServerLaneControlWindow.GetNextBatchEstimatedBytes())
			&& ServerLaneControlWindow.TryBuildBatch(LaneControlBatch))
		{
			ClientReceiveMeteorLaneControlBatch(MoveTemp(LaneControlBatch));
			LaneControlBatch = {};
		}
		while (ServerScheduler->TryDispatch())
		{
		}
	}
	const double NowSeconds = GetWorld()->GetTimeSeconds();
	if (Controller->IsLocalController() && !Controller->HasAuthority())
	{
		DrainClientSourceCausalGates();
	}
	CSV_CUSTOM_STAT(MeteorSourceGate, PendingSourceCommits,
		ClientPendingSourceCommits.Num() - ClientPendingSourceCommitHead, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(MeteorSourceGate, AwaitingSourceProjection,
		ClientAwaitingSourceProjection.Num() - ClientAwaitingSourceProjectionHead, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(MeteorSourceGate, SourceCommitsThisFrame,
		LastClientSourceCommitCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(MeteorSourceGate, ReleasedLanesThisFrame,
		LastClientSourceReleasedLaneCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(MeteorSourceGate, GateMilliseconds,
		LastClientSourceGateMilliseconds, ECsvCustomStatOp::Set);
	if (NowSeconds >= NextTelemetrySeconds)
	{
		NextTelemetrySeconds = NowSeconds + 1.0;
		if (Controller->HasAuthority() && ServerScheduler
			&& (ServerOfferedPageCount > 0 || !ServerPayloads.IsEmpty()))
		{
			UE_LOG(LogElementSandboxMeteorStream, Display,
				TEXT("Meteor Stream 服务端：Offer=%llu 页/%llu Lane/%.2fMiB，Activate=%llu Lane，待 ACK 页=%d，队列=%d 段/%.2fMiB，在途=%d；控制待发=%d，结算待发=%d，在途批=%d。"),
				ServerOfferedPageCount,
				ServerOfferedLaneCount,
				static_cast<double>(ServerOfferedByteCount) / (1024.0 * 1024.0),
				ServerActivatedLaneCount,
				ServerPayloads.Num(),
				ServerScheduler->GetQueuedSegmentCount(),
				static_cast<double>(ServerScheduler->GetQueuedBytes()) / (1024.0 * 1024.0),
				ServerScheduler->GetInFlightCount(),
				ServerLaneControlWindow.GetPendingRecordCount(),
				ServerLaneControlWindow.GetPendingSettlementCount(),
				ServerLaneControlWindow.GetInFlightBatchCount());
		}
		else if (Controller->IsLocalController()
			&& (ClientDecodedPageCount > 0 || !ClientAssemblies.IsEmpty()))
		{
			UE_LOG(LogElementSandboxMeteorStream, Display,
				TEXT("Meteor Stream 客户端：Decoded=%llu 页/%llu Lane，Applied=%llu Lane，组装中=%d。"),
				ClientDecodedPageCount,
				ClientDecodedLaneCount,
				ClientAppliedLaneCount,
				ClientAssemblies.Num());
		}
	}
	if (Controller->IsLocalController() && !Controller->HasAuthority()
		&& NowSeconds >= NextInterestSendSeconds)
	{
		FVector Location;
		FRotator Rotation;
		Controller->GetPlayerViewPoint(Location, Rotation);
		ServerSubmitMeteorCameraInterest(Location, Rotation.Vector(), Controller->PlayerCameraManager
			? Controller->PlayerCameraManager->GetFOVAngle() : 90.0f);
		NextInterestSendSeconds = NowSeconds + 0.25;
	}
}

void UMeteorStreamingComponent::HandlePagePrepared(const FMeteorTrajectoryPage& Page)
{
	if (!ServerScheduler || !Page.IsValid() || !GetWorld()) return;
	TArray<uint8> Bytes;
	if (!Page.SerializeToBytes(Bytes)) return;
	FMeteorNetPageId Id{Page.BurstId.Value, Page.PageId, Page.Revision};
	FMeteorNetPageOffer Offer;
	Offer.Id = Id;
	Offer.TotalBytes = Bytes.Num();
	Offer.SegmentCount = static_cast<uint16>(FMath::DivideAndRoundUp(Bytes.Num(), DefaultSegmentBytes));
	Offer.PayloadHash = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
	const double DistanceSquared = FVector::DistSquared(
		ServerInterestLocation,
		FVector(Page.PageOrigin + FVector3d(Page.SweptBounds.GetCenter())));
	const UMeteorWorldSubsystem* Meteor = GetWorld()->GetSubsystem<UMeteorWorldSubsystem>();
	const FMeteorRuntimeConfig Config = Meteor ? Meteor->GetRuntimeConfig() : FMeteorRuntimeConfig{};
	const double UrgentLeadSeconds = Config.NetworkLeadSeconds
		+ Config.EncodingEstimateSeconds + Config.QueueSafetySeconds;
	const bool bUrgent = Page.ValidFromSeconds <= GetWorld()->GetTimeSeconds() + UrgentLeadSeconds
		|| DistanceSquared < FMath::Square(200000.0);
	const TWeakObjectPtr<UMeteorStreamingComponent> WeakThis(this);
	if (!ServerScheduler->Enqueue(
		bUrgent ? ETransferClass::MeteorUrgent : ETransferClass::MeteorBackground,
		ToPayloadId(Id),
		Bytes,
		DefaultSegmentBytes,
		[WeakThis, Id](FSegment&& Segment)
		{
			UMeteorStreamingComponent* Component = WeakThis.Get();
			FServerPayloadState* State = Component
				? Component->ServerPayloads.Find(Id) : nullptr;
			if (!Component || !State)
			{
				return;
			}
			// Offer 与首段一起经过传输准入，准备大量页或 Late Join 时不能先灌满可靠通道。
			if (!State->bOfferSent)
			{
				Component->ClientOfferMeteorPage(State->Offer);
				State->bOfferSent = true;
			}
			FMeteorNetPageSegment NetSegment;
			NetSegment.Id = Id;
			NetSegment.SegmentIndex = Segment.SegmentIndex;
			NetSegment.SegmentCount = Segment.SegmentCount;
			NetSegment.PayloadHash = Segment.PayloadHash;
			NetSegment.Bytes = MoveTemp(Segment.Bytes);
			Component->ClientReceiveMeteorSegment(MoveTemp(NetSegment));
		})) return;
	ServerPayloads.Add(Id, {Offer, 0});
	++ServerOfferedPageCount;
	ServerOfferedLaneCount += Page.Num();
	ServerOfferedByteCount += Bytes.Num();
}

void UMeteorStreamingComponent::HandlePageActivated(const FMeteorTrajectoryActivation& Activation)
{
	if (!Activation.IsValid()) return;
	FMeteorNetPageActivation NetActivation;
	NetActivation.Id = {Activation.BurstId.Value, Activation.PageId, Activation.Revision};
	NetActivation.AuthorityStartTimeSeconds = Activation.AuthorityStartTimeSeconds;
	NetActivation.SourceWorldEntityId = Activation.SourceWorldEntityId;
	NetActivation.SourceTombstoneRevision = Activation.SourceTombstoneRevision;
	NetActivation.Ordinals = Activation.Ordinals;
	ServerActivatedLaneCount += Activation.Ordinals.Num();
	ServerLaneControlWindow.Enqueue(MoveTemp(NetActivation), false);
}

void UMeteorStreamingComponent::HandlePageCanceled(const FMeteorTrajectoryActivation& Cancellation)
{
	if (!Cancellation.IsCancellationValid()) return;
	FMeteorNetPageActivation NetCancellation;
	NetCancellation.Id = {Cancellation.BurstId.Value, Cancellation.PageId, Cancellation.Revision};
	NetCancellation.Ordinals = Cancellation.Ordinals;
	ServerLaneControlWindow.Enqueue(MoveTemp(NetCancellation), true);
}

void UMeteorStreamingComponent::HandleSettlementPublished(
	const TConstArrayView<FMeteorSettlementMapping> Mappings)
{
	TArray<FMeteorNetSettlement> NetMappings;
	NetMappings.Reserve(Mappings.Num());
	for (const FMeteorSettlementMapping& Mapping : Mappings)
	{
		NetMappings.Add({Mapping.Debris.BurstId.Value, Mapping.Debris.DebrisOrdinal, Mapping.WorldEntityId});
	}
	ServerLaneControlWindow.EnqueueSettlements(NetMappings);
}

void UMeteorStreamingComponent::ClientOfferMeteorPage_Implementation(const FMeteorNetPageOffer Offer)
{
	if (!Offer.Id.IsValid() || Offer.TotalBytes <= 0 || Offer.TotalBytes > 4 * 1024 * 1024
		|| Offer.SegmentCount == 0 || Offer.SegmentCount > 512) return;
	FClientAssembly& Assembly = ClientAssemblies.FindOrAdd(Offer.Id);
	// 相同 Offer 重放不能抹掉已收到的分段；同版本不同哈希也不能替换正在接收的页。
	if (Assembly.Offer.Id.IsValid()) return;
	Assembly.Offer = Offer;
	Assembly.Segments.SetNum(Offer.SegmentCount);
	Assembly.Received.Init(false, Offer.SegmentCount);
}

void UMeteorStreamingComponent::ClientReceiveMeteorSegment_Implementation(FMeteorNetPageSegment Segment)
{
	FClientAssembly* Assembly = ClientAssemblies.Find(Segment.Id);
	if (!Assembly || Segment.SegmentCount != Assembly->Offer.SegmentCount
		|| Segment.PayloadHash != Assembly->Offer.PayloadHash
		|| Segment.SegmentIndex >= Segment.SegmentCount || Segment.Bytes.IsEmpty()
		|| Segment.Bytes.Num() > DefaultSegmentBytes) return;
	if (!Assembly->Received[Segment.SegmentIndex])
	{
		Assembly->Segments[Segment.SegmentIndex] = MoveTemp(Segment.Bytes);
		Assembly->Received[Segment.SegmentIndex] = true;
	}
	ServerAckMeteorSegment(Segment.Id, Segment.SegmentIndex);
	TryFinalizeClientPage(Segment.Id);
}

void UMeteorStreamingComponent::ClientReceiveMeteorLaneControlBatch_Implementation(
	FMeteorNetLaneControlBatch Batch)
{
	if (Batch.Sequence == 0 || Batch.Controls.Num() > FMeteorLaneControlSendWindow::MaximumRecordsPerBatch
		|| Batch.Settlements.Num() > FMeteorLaneControlSendWindow::MaximumSettlementsPerBatch)
	{
		return;
	}
	for (FMeteorNetLaneControl& Control : Batch.Controls)
	{
		if (Control.bCancellation)
		{
			ApplyClientCancellation(MoveTemp(Control.Activation));
		}
		else
		{
			QueueClientActivation(MoveTemp(Control.Activation));
		}
	}
	PublishClientSettlements(Batch.Settlements);
	ServerAckMeteorLaneControlBatch(Batch.Sequence);
}

void UMeteorStreamingComponent::QueueClientActivation(FMeteorNetPageActivation Activation)
{
	if (!Activation.Id.IsValid() || Activation.Ordinals.IsEmpty()
		|| !Activation.SourceWorldEntityId.IsSet() || Activation.SourceTombstoneRevision == 0
		|| !FMath::IsFinite(Activation.AuthorityStartTimeSeconds)
		|| Activation.AuthorityStartTimeSeconds < 0.0)
	{
		return;
	}
	FClientSourceCausalGate& Gate = ClientSourceCausalGates.FindOrAdd(
		Activation.SourceWorldEntityId);
	Gate.TombstoneRevision = FMath::Max(
		Gate.TombstoneRevision, Activation.SourceTombstoneRevision);
	Gate.PendingActivations.Add(MoveTemp(Activation));
	if (Gate.AppliedTombstoneRevision < Gate.TombstoneRevision && !Gate.bCommitQueued)
	{
		Gate.bCommitQueued = true;
		ClientPendingSourceCommits.Add(Gate.PendingActivations.Last().SourceWorldEntityId);
	}
}

void UMeteorStreamingComponent::DrainClientSourceCausalGates()
{
	LastClientSourceCommitCount = 0;
	LastClientSourceReleasedLaneCount = 0;
	const double GateStartSeconds = FPlatformTime::Seconds();
	UWorld* World = GetWorld();
	UWorldStorageSubsystem* Storage = World ? World->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!World || !Storage)
	{
		LastClientSourceGateMilliseconds = 0.0;
		return;
	}

	UBuildingWorldSubsystem* Building = World->GetSubsystem<UBuildingWorldSubsystem>();
	TArray<FWorldNetworkEntityRemoval, TInlineAllocator<MaximumSourceCommitBatchSize>> Removals;
	TArray<FWorldEntityId, TInlineAllocator<MaximumSourceCommitBatchSize>> SourceIds;
	Removals.Reserve(MaximumSourceCommitBatchSize);
	SourceIds.Reserve(MaximumSourceCommitBatchSize);
	while (ClientPendingSourceCommitHead < ClientPendingSourceCommits.Num()
		&& Removals.Num() < MaximumSourceCommitBatchSize)
	{
		const FWorldEntityId SourceId =
			ClientPendingSourceCommits[ClientPendingSourceCommitHead++];
		FClientSourceCausalGate* Gate = ClientSourceCausalGates.Find(SourceId);
		if (!Gate || !Gate->bCommitQueued)
		{
			continue;
		}
		Gate->bCommitQueued = false;
		if (!Gate->BuildingEntity.IsSet() && Building)
		{
			Gate->BuildingEntity = Building->FindEntity(SourceId);
		}
		SourceIds.Add(SourceId);
		Removals.Add({SourceId, Gate->TombstoneRevision, true});
	}
	LastClientSourceCommitCount = SourceIds.Num();
	if (!Removals.IsEmpty())
	{
		const bool bBatchApplied = ApplyMeteorSourceTombstones(*Storage, Removals);
		for (int32 Index = 0; Index < SourceIds.Num(); ++Index)
		{
			const FWorldEntityId SourceId = SourceIds[Index];
			FClientSourceCausalGate* Gate = ClientSourceCausalGates.Find(SourceId);
			if (!Gate)
			{
				continue;
			}
			// 批次失败可能来自一个坏 Revision；WorldStorage 的 GameplayDestroy 是幂等的，
			// 只在异常路径逐项回退以隔离坏源，正常路径始终只有一次领域 Batch 提交。
			const bool bApplied = bBatchApplied || ApplyMeteorSourceTombstone(
				*Storage, SourceId, Removals[Index].StateRevision);
			if (!bApplied)
			{
				UE_LOG(LogElementSandboxMeteorStream, Error,
					TEXT("Meteor Source Tombstone 被拒绝：Source=%llu Revision=%u；对应 Lane 不会显示。"),
					SourceId.GetValue(), Removals[Index].StateRevision);
				ClientSourceCausalGates.Remove(SourceId);
				continue;
			}
			Gate = ClientSourceCausalGates.Find(SourceId);
			if (!Gate)
			{
				continue;
			}
			Gate->AppliedTombstoneRevision = Removals[Index].StateRevision;
			if (!Gate->bProjectionQueued)
			{
				Gate->bProjectionQueued = true;
				ClientAwaitingSourceProjection.Add(SourceId);
			}
		}
	}
	CompactConsumedQueue(ClientPendingSourceCommits, ClientPendingSourceCommitHead);

	USettlementTreePresentationWorldSubsystem* Trees =
		World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	const double ProjectionStartSeconds = FPlatformTime::Seconds();
	const int32 ProjectionEnd = FMath::Min(
		ClientAwaitingSourceProjection.Num(),
		ClientAwaitingSourceProjectionHead + MaximumSourceProjectionChecksPerFrame);
	while (ClientAwaitingSourceProjectionHead < ProjectionEnd)
	{
		const FWorldEntityId SourceId =
			ClientAwaitingSourceProjection[ClientAwaitingSourceProjectionHead++];
		FClientSourceCausalGate* Gate = ClientSourceCausalGates.Find(SourceId);
		if (!Gate || !Gate->bProjectionQueued)
		{
			continue;
		}
		Gate->bProjectionQueued = false;
		if (Gate->AppliedTombstoneRevision < Gate->TombstoneRevision)
		{
			continue;
		}
		const bool bBuildingProjectionExists = Building && Gate->BuildingEntity.IsSet()
			&& Building->IsEntityPresentationResident(Gate->BuildingEntity);
		const bool bTreeProjectionExists = Trees && Trees->HasWorldEntityProjection(SourceId);
		if (bBuildingProjectionExists || bTreeProjectionExists)
		{
			Gate->bProjectionQueued = true;
			ClientAwaitingSourceProjection.Add(SourceId);
		}
		else
		{
			const uint64 AppliedBefore = ClientAppliedLaneCount;
			for (const FMeteorNetPageActivation& Pending : Gate->PendingActivations)
			{
				PublishClientActivation(Pending);
			}
			LastClientSourceReleasedLaneCount += static_cast<int32>(
				ClientAppliedLaneCount - AppliedBefore);
			ClientSourceCausalGates.Remove(SourceId);
		}
		if (FPlatformTime::Seconds() - ProjectionStartSeconds
			>= SourceProjectionCheckBudgetSeconds)
		{
			break;
		}
	}
	CompactConsumedQueue(
		ClientAwaitingSourceProjection, ClientAwaitingSourceProjectionHead);
	LastClientSourceGateMilliseconds =
		(FPlatformTime::Seconds() - GateStartSeconds) * 1000.0;
}

void UMeteorStreamingComponent::PublishClientActivation(
	const FMeteorNetPageActivation& Activation)
{
	if (ClientCanceledPages.Contains(Activation.Id))
	{
		return;
	}
	FClientAssembly& Assembly = ClientAssemblies.FindOrAdd(Activation.Id);
	FMeteorTrajectoryActivation RuntimeActivation;
	RuntimeActivation.BurstId.Value = Activation.Id.BurstId;
	RuntimeActivation.PageId = Activation.Id.PageId;
	RuntimeActivation.Revision = Activation.Id.Revision;
	RuntimeActivation.SourceWorldEntityId = Activation.SourceWorldEntityId;
	RuntimeActivation.SourceTombstoneRevision = Activation.SourceTombstoneRevision;
	RuntimeActivation.AuthorityStartTimeSeconds = Activation.AuthorityStartTimeSeconds;
	for (const uint32 Ordinal : Activation.Ordinals)
	{
		if (Ordinal == MAX_uint32 || Assembly.ActivatedOrdinals.Contains(Ordinal)
			|| Assembly.CanceledOrdinals.Contains(Ordinal))
		{
			continue;
		}
		Assembly.ActivatedOrdinals.Add(Ordinal);
		RuntimeActivation.Ordinals.Add(Ordinal);
	}
	if (!RuntimeActivation.Ordinals.IsEmpty())
	{
		if (UMeteorClientWorldSubsystem* Client =
			GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>())
		{
			Client->QueueTrajectoryActivation(RuntimeActivation);
			ClientAppliedLaneCount += RuntimeActivation.Ordinals.Num();
		}
	}
	TryFinalizeClientPage(Activation.Id);
}

void UMeteorStreamingComponent::ClientCancelMeteorPage_Implementation(const FMeteorNetPageId Id)
{
	if (Id.IsValid())
	{
		ClientCanceledPages.Add(Id);
	}
	if (UMeteorClientWorldSubsystem* Client = GetWorld() ? GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>() : nullptr;
		Client && Id.IsValid())
	{
		Client->QueueTrajectoryPageCancellation({Id.BurstId}, Id.PageId, Id.Revision);
	}
	ClientAssemblies.Remove(Id);
}

void UMeteorStreamingComponent::ApplyClientCancellation(FMeteorNetPageActivation Cancellation)
{
	if (!Cancellation.Id.IsValid() || Cancellation.Ordinals.IsEmpty()) return;
	FClientAssembly& Assembly = ClientAssemblies.FindOrAdd(Cancellation.Id);
	FMeteorTrajectoryActivation RuntimeCancellation;
	RuntimeCancellation.BurstId.Value = Cancellation.Id.BurstId;
	RuntimeCancellation.PageId = Cancellation.Id.PageId;
	RuntimeCancellation.Revision = Cancellation.Id.Revision;
	for (const uint32 Ordinal : Cancellation.Ordinals)
	{
		if (Ordinal == MAX_uint32 || Assembly.ActivatedOrdinals.Contains(Ordinal)) continue;
		Assembly.CanceledOrdinals.Add(Ordinal);
		RuntimeCancellation.Ordinals.Add(Ordinal);
	}
	if (!RuntimeCancellation.Ordinals.IsEmpty())
	{
		if (UMeteorClientWorldSubsystem* Client = GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>())
		{
			Client->QueueTrajectoryCancellation(RuntimeCancellation);
		}
	}
	TryFinalizeClientPage(Cancellation.Id);
}

void UMeteorStreamingComponent::PublishClientSettlements(
	const TConstArrayView<FMeteorNetSettlement> Settlements)
{
	UMeteorClientWorldSubsystem* Client = GetWorld()
		? GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>() : nullptr;
	if (!Client || Settlements.IsEmpty())
	{
		return;
	}
	TArray<FMeteorSettlementMapping> Mappings;
	Mappings.Reserve(Settlements.Num());
	for (const FMeteorNetSettlement& Settlement : Settlements)
	{
		if (Settlement.IsValid())
		{
			Mappings.Add({{{Settlement.BurstId}, Settlement.DebrisOrdinal}, Settlement.WorldEntityId});
		}
	}
	if (!Mappings.IsEmpty())
	{
		Client->QueueSettlementMappings(Mappings);
	}
}

void UMeteorStreamingComponent::ServerAckMeteorSegment_Implementation(
	const FMeteorNetPageId Id,
	const uint16 SegmentIndex)
{
	if (!ServerScheduler || !Id.IsValid() || !ServerScheduler->Acknowledge(ToPayloadId(Id), SegmentIndex)) return;
	if (FServerPayloadState* State = ServerPayloads.Find(Id))
	{
		++State->AckedSegments;
		if (State->AckedSegments >= State->Offer.SegmentCount) ServerPayloads.Remove(Id);
	}
}

void UMeteorStreamingComponent::ServerAckMeteorLaneControlBatch_Implementation(
	const uint32 Sequence)
{
	ServerLaneControlWindow.Acknowledge(Sequence);
}

void UMeteorStreamingComponent::ServerSubmitMeteorCameraInterest_Implementation(
	const FVector_NetQuantize100 Location,
	const FVector_NetQuantizeNormal Direction,
	const float FieldOfViewDegrees)
{
	const APawn* Pawn = Cast<APlayerController>(GetOwner()) ? Cast<APlayerController>(GetOwner())->GetPawn() : nullptr;
	if (!Pawn || FVector(Location).ContainsNaN() || FVector(Direction).ContainsNaN()) return;
	ServerInterestLocation = FVector::DistSquared(Location, Pawn->GetActorLocation()) <= FMath::Square(5000.0)
		? FVector(Location) : Pawn->GetActorLocation();
	ServerInterestDirection = FVector(Direction).GetSafeNormal();
	ServerInterestFov = FMath::Clamp(FieldOfViewDegrees, 30.0f, 140.0f);
}

void UMeteorStreamingComponent::TryFinalizeClientPage(const FMeteorNetPageId& Id)
{
	FClientAssembly* Assembly = ClientAssemblies.Find(Id);
	if (!Assembly) return;
	if (!Assembly->DecodedPage)
	{
		if (!Assembly->Offer.Id.IsValid() || Assembly->Received.IsEmpty()
			|| Assembly->Received.Find(false) != INDEX_NONE) return;
		TArray<uint8> Bytes;
		Bytes.Reserve(Assembly->Offer.TotalBytes);
		for (const TArray<uint8>& Segment : Assembly->Segments) Bytes.Append(Segment);
		if (Bytes.Num() != Assembly->Offer.TotalBytes
			|| FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()) != Assembly->Offer.PayloadHash)
		{
			ClientAssemblies.Remove(Id);
			return;
		}
		TSharedPtr<FMeteorTrajectoryPage> Page = MakeShared<FMeteorTrajectoryPage>();
		if (!FMeteorTrajectoryPage::DeserializeFromBytes(Bytes, *Page)
			|| Page->BurstId.Value != Id.BurstId || Page->PageId != Id.PageId || Page->Revision != Id.Revision)
		{
			ClientAssemblies.Remove(Id);
			return;
		}
		Assembly->DecodedPage = MoveTemp(Page);
		++ClientDecodedPageCount;
		ClientDecodedLaneCount += Assembly->DecodedPage->Num();
		Assembly->Segments.Reset();
		Assembly->Received.Reset();
		if (UMeteorClientWorldSubsystem* Client = GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>())
		{
			Client->QueuePreparedTrajectoryPage(Assembly->DecodedPage);
		}
	}
	// Payload 先到时也必须在最后一个控制消息到达后释放，不能被上面的“已解码”提前返回截断。
	for (uint32 Ordinal : Assembly->DecodedPage->Ordinals)
		if (!Assembly->ActivatedOrdinals.Contains(Ordinal) && !Assembly->CanceledOrdinals.Contains(Ordinal)) return;
	ClientAssemblies.Remove(Id);
}
