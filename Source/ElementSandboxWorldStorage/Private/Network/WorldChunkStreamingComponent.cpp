#include "Network/WorldChunkStreamingComponent.h"

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "ElementSandboxWorldStorage.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NetBulkTransferActorChannel.h"
#include "NetBulkTransferScheduler.h"
#include "Network/WorldChunkActivationReadiness.h"
#include "Network/WorldChunkClientCache.h"
#include "Network/WorldChunkLiveDeltaFlowControl.h"
#include "Network/WorldChunkOfferFlowControl.h"
#include "Network/WorldChunkStreamingSessions.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Storage/WorldChunkCodec.h"

using namespace UE::ElementSandbox::WorldStorage::Private;
using namespace UE::ElementSandbox::NetBulk;

namespace
{
constexpr uint32 WorldStorageTransferDomain = 0x57535447; // WSTG

FString BuildServerFingerprint(const UWorld& World)
{
	FString Address = TEXT("Loopback");
	if (const UNetDriver* NetDriver = World.GetNetDriver())
	{
		if (UNetConnection* Connection = NetDriver->ServerConnection)
		{
			// 临时 Loopback 端口每次启动都会变化；Fingerprint 只标识服务器主机，
			// WorldId 再区分具体世界，才能让单机磁盘缓存跨会话复用。
			Address = Connection->LowLevelGetRemoteAddress(false);
		}
	}
	FTCHARToUTF8 Utf8(*Address);
	uint8 Digest[FSHA1::DigestSize] = {};
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	return BytesToHex(Digest, 12);
}

double ChunkTransferPriorityScore(const FWorldChunkCoord& Coord, const FVector& SourceLocation,
								  const FVector& SourceForward)
{
	const FVector ChunkCenter = Coord.GetWorldMinimum() + FVector(FWorldChunkCoord::EdgeCentimeters * 0.5);
	const FVector ToChunk = ChunkCenter - SourceLocation;
	const double SquaredDistance = ToChunk.SquaredLength();
	if (SquaredDistance <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	const double Facing = FVector::DotProduct(ToChunk.GetSafeNormal(),
											  SourceForward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector));
	return SquaredDistance * (1.25 - 0.25 * FMath::Clamp(Facing, -1.0, 1.0));
}

bool IsActivationCore(const FWorldChunkCoord& Coord, const FWorldChunkCoord& Center)
{
	return FMath::Abs(Coord.X - Center.X) <= 1 && FMath::Abs(Coord.Y - Center.Y) <= 1 &&
		   FMath::Abs(Coord.Z - Center.Z) <= 1;
}
} // namespace

UWorldChunkStreamingComponent::UWorldChunkStreamingComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

UWorldChunkStreamingComponent::~UWorldChunkStreamingComponent() = default;

void UWorldChunkStreamingComponent::SetBulkTransferScheduler(
	TSharedPtr<FConnectionScheduler> InScheduler)
{
	check(!HasBegunPlay());
	BulkTransferScheduler = MoveTemp(InScheduler);
}

void UWorldChunkStreamingComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!BulkTransferScheduler)
	{
		BulkTransferScheduler = MakeShared<FConnectionScheduler>();
	}
	BindActorChannelTransport(*BulkTransferScheduler, GetOwner());
	Runtime = MakePimpl<FWorldChunkStreamingRuntime>();
	Runtime->BeginPlaySeconds = FPlatformTime::Seconds();
	UWorld* World = GetWorld();
	UWorldStorageSubsystem* Storage = World ? World->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	WorldStorage = Storage;
	if (!World || !Storage)
	{
		SetComponentTickEnabled(false);
		return;
	}
	Runtime->Client.ServerFingerprint = BuildServerFingerprint(*World);
	if (Storage->IsAuthorityStorage())
	{
		MutationHandle =
			Storage->OnAuthorityMutation().AddUObject(this, &UWorldChunkStreamingComponent::HandleAuthorityMutation);
		OfferAccumulator = OfferIntervalSeconds;
		ApplyAuthorityActivationGate(false);
	}
	else
	{
		ApplyClientActivationGate(false);
		ServerMarkChunkStreamingEndpointReady();
	}
	UpdateAuthoritativeResidencySource(true);
}

void UWorldChunkStreamingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Runtime && BulkTransferScheduler)
	{
		for (const TPair<FWorldChunkCoord, FOutgoingChunk>& Pair : Runtime->Server.OutgoingChunks)
		{
			if (Pair.Value.BulkPayloadId.IsValid())
			{
				BulkTransferScheduler->Cancel(Pair.Value.BulkPayloadId);
			}
		}
	}
	if (UWorldStorageSubsystem* Storage = WorldStorage.Get())
	{
		if (MutationHandle.IsValid())
		{
			Storage->OnAuthorityMutation().Remove(MutationHandle);
			MutationHandle.Reset();
		}
		if (ResidencySource.IsSet())
		{
			Storage->UnregisterResidencySource(ResidencySource);
			ResidencySource = {};
		}
	}
	ApplyAuthorityActivationGate(true);
	ApplyClientActivationGate(true);
	WorldStorage.Reset();
	Runtime.Reset();
	BulkTransferScheduler.Reset();
	Super::EndPlay(EndPlayReason);
}

void UWorldChunkStreamingComponent::TickComponent(const float DeltaTime, const ELevelTick TickType,
												  FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Runtime || TickType == LEVELTICK_TimeOnly)
	{
		return;
	}
	UpdateAuthoritativeResidencySource(false);
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Storage || !Storage->IsAuthorityStorage())
	{
		PumpClientLiveDeltaBatch();
		PumpClientCacheLookups();
		return;
	}
	OfferAccumulator += DeltaTime;
	if (OfferAccumulator >= OfferIntervalSeconds)
	{
		OfferAccumulator = FMath::Fmod(OfferAccumulator, OfferIntervalSeconds);
		RefreshServerOffers();
	}
	PumpServerOfferQueue();
	PumpServerLiveDeltas();
	PumpServerPayload();
	RefreshServerStreamingStats();
}

FWorldChunkStreamingStats UWorldChunkStreamingComponent::GetStreamingStats() const
{
	return Runtime ? Runtime->Stats : FWorldChunkStreamingStats();
}

bool UWorldChunkStreamingComponent::IsAuthorityChunkReadyForLiveMutation(const FWorldChunkCoord Coord) const
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage || !Storage->IsAuthorityStorage())
	{
		return false;
	}
	const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
	return Subscription && Subscription->bSnapshotAcknowledged &&
		   Storage->IsAuthorityChunkReadyForActivation(Coord);
}

void UWorldChunkStreamingComponent::NotifyPawnChanged()
{
	if (!Runtime)
	{
		return;
	}
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Storage)
	{
		return;
	}
	if (Storage->IsAuthorityStorage())
	{
		ApplyAuthorityActivationGate(Runtime->Activation.bInitialActivationGateSatisfied);
		if (Cast<APlayerController>(GetOwner()) && Cast<APlayerController>(GetOwner())->GetPawn())
		{
			UpdateAuthoritativeResidencySource(true);
		}
	}
	else
	{
		ApplyClientActivationGate(Runtime->Stats.bActivationCoreReady);
		UpdateAuthoritativeResidencySource(true);
	}
}

void UWorldChunkStreamingComponent::UpdateAuthoritativeResidencySource(const bool bForce)
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!Storage || !PlayerController || (!Storage->IsAuthorityStorage() && !PlayerController->IsLocalController()))
	{
		return;
	}
	const APawn* Pawn = PlayerController->GetPawn();
	if (!Pawn)
	{
		return;
	}
	if (Storage->IsAuthorityStorage() && !Runtime->Activation.bInitialActivationGateSatisfied)
	{
		ApplyAuthorityActivationGate(false);
	}
	const FVector Location = Pawn->GetActorLocation();
	const FVector Forward = PlayerController->GetControlRotation().Vector();
	const FWorldChunkCoord NewCenter = FWorldChunkCoord::FromWorldLocation(Location);
	const bool bCenterChanged = ResidencySource.IsSet() && NewCenter != Runtime->Observation.CurrentCenter;
	Runtime->Observation.CurrentCenter = NewCenter;
	Runtime->Observation.CurrentLocation = Location;
	Runtime->Observation.CurrentForward = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	if (!ResidencySource.IsSet())
	{
		ResidencySource = Storage->RegisterResidencySource(Location, Runtime->Observation.CurrentForward);
	}
	else
	{
		Storage->UpdateResidencySource(ResidencySource, Location, Runtime->Observation.CurrentForward, bForce);
	}
	if (Storage->IsAuthorityStorage() && (bForce || bCenterChanged))
	{
		OfferAccumulator = OfferIntervalSeconds;
		if (bCenterChanged)
		{
			UE_LOG(LogTemp, Display,
				   TEXT("Authority Residency Center 后台迁移：(%d,%d,%d)，Pawn=(%.0f,%.0f,%.0f)。"),
				   NewCenter.X, NewCenter.Y, NewCenter.Z, Location.X, Location.Y, Location.Z);
		}
	}
}

void UWorldChunkStreamingComponent::RefreshServerOffers()
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage || !ResidencySource.IsSet())
	{
		return;
	}
	TArray<FWorldChunkOffer> CurrentOffers;
	TSet<FWorldChunkCoord> CurrentCoords;
	TArray<FWorldChunkCoord> DenseActivationCore;
	BuildDenseActivationCore(Runtime->Observation.CurrentCenter, DenseActivationCore);
	Runtime->Server.ActivationCoreChunks.Reset();
	for (const FWorldChunkCoord& Coord : DenseActivationCore)
	{
		Runtime->Server.ActivationCoreChunks.Add(Coord);
	}
	TSet<FWorldChunkCoord> AcknowledgedChunks;
	AcknowledgedChunks.Reserve(Runtime->Server.ServerSubscriptions.Num());
	for (TPair<FWorldChunkCoord, FServerChunkSubscription>& Pair : Runtime->Server.ServerSubscriptions)
	{
		if (Pair.Value.bSnapshotAcknowledged)
		{
			AcknowledgedChunks.Add(Pair.Key);
			Pair.Value.Offer.bActivationCore = Runtime->Server.ActivationCoreChunks.Contains(Pair.Key);
		}
	}
	Storage->GetRelevantChunkOffers(
		ResidencySource, DenseActivationCore, AcknowledgedChunks, CurrentOffers, CurrentCoords);
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FWorldChunkOffer& SourceOffer : CurrentOffers)
	{
		FWorldChunkOffer Offer = SourceOffer;
		Offer.bActivationCore = Runtime->Server.ActivationCoreChunks.Contains(Offer.Coord);
		FServerChunkSubscription* Existing = Runtime->Server.ServerSubscriptions.Find(Offer.Coord);
		const bool bCoreClassificationChanged = Existing && Existing->Offer.bActivationCore != Offer.bActivationCore;
		if (!Existing || (!Existing->bSnapshotAcknowledged &&
						  (Existing->Offer.Revision != Offer.Revision ||
						   Existing->Offer.ContentHash != Offer.ContentHash || bCoreClassificationChanged)))
		{
			FServerChunkSubscription& Subscription = Runtime->Server.ServerSubscriptions.FindOrAdd(Offer.Coord);
			Subscription.Offer = Offer;
			Subscription.bSnapshotAcknowledged = false;
			Subscription.SnapshotFailureCount = 0;
			Subscription.LastOfferSentSeconds = -DBL_MAX;
			Subscription.OfferSendCount = 0;
			Subscription.OfferFlow.Reset();
			if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Offer.Coord);
				Outgoing && BulkTransferScheduler)
			{
				BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
			}
			Runtime->Server.OutgoingChunks.Remove(Offer.Coord);
			Runtime->Server.PendingOffersToSend.Add(Offer.Coord, Offer);
		}
		FServerChunkSubscription& Subscription = Runtime->Server.ServerSubscriptions.FindChecked(Offer.Coord);
		const bool bSnapshotPipelineActive = Runtime->Server.OutgoingChunks.Contains(Offer.Coord) ||
											 Runtime->Server.SnapshotPreparations.Contains(Offer.Coord) ||
											 Runtime->Server.PendingSnapshotRequests.Contains(Offer.Coord);
		if (!Subscription.bSnapshotAcknowledged &&
			Subscription.OfferFlow.ShouldRetryOffer(
				bSnapshotPipelineActive, Runtime->Server.PendingOffersToSend.Contains(Offer.Coord), NowSeconds,
				Subscription.LastOfferSentSeconds, OfferResponseTimeoutSeconds))
		{
			// RPC 可能在 PlayerController 的 Owner Channel 完全建立前被调用；只有
			// Have/Request 才是应用层确认，超时后必须重发同一 Revision 的 Offer。
			Runtime->Server.PendingOffersToSend.Add(Offer.Coord, Subscription.Offer);
		}
	}
	for (auto It = Runtime->Server.ServerSubscriptions.CreateIterator(); It; ++It)
	{
		if (!CurrentCoords.Contains(It.Key()))
		{
			if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(It.Key());
				Outgoing && BulkTransferScheduler)
			{
				BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
			}
			Runtime->Server.OutgoingChunks.Remove(It.Key());
			Runtime->Server.PendingSnapshotRequests.Remove(It.Key());
			Runtime->Server.PendingOffersToSend.Remove(It.Key());
			Runtime->Server.LiveDeltaQueue.RemoveChunk(It.Key());
			It.RemoveCurrent();
		}
	}
	RefreshServerStreamingStats();
	const FWorldStorageManifestInfo Manifest = Storage->GetManifestInfo();
	Runtime->Stats.CompleteStructureCount = Manifest.CompleteStructureCount;
	Runtime->Stats.BuildingEntityCount = Manifest.BuildingEntityCount;
	Runtime->Stats.WorldObjectEntityCount = Manifest.WorldObjectEntityCount;
	const FWorldStorageRuntimeStats StorageStats = Storage->GetRuntimeStats();
	Runtime->Stats.AuthorityResidentEntityCount = StorageStats.ResidentEntityCount;
	Runtime->Stats.AuthorityResidentChunkCount = StorageStats.ResidentChunkCount;
	Runtime->Stats.AuthorityPendingLoadCount = StorageStats.PendingLoadCount;
	Runtime->Stats.AuthorityPendingInjectionCount = StorageStats.PendingInjectionCount;
	Runtime->Stats.AuthorityDirtyEntityCount = StorageStats.DirtyEntityCount;
	Runtime->Stats.AuthorityAwakePhysicsPinnedEntityCount = StorageStats.AwakePhysicsPinnedEntityCount;
	Runtime->Stats.AuthorityOldestAwakePhysicsPinSeconds = StorageStats.OldestAwakePhysicsPinSeconds;
	Runtime->Stats.AuthorityLastInjectionMilliseconds = StorageStats.LastInjectionMilliseconds;
	Runtime->Stats.AuthorityLastStepMilliseconds = StorageStats.LastAuthorityStepMilliseconds;
	Runtime->Stats.WorldSimulationTimeMilliseconds = StorageStats.WorldSimulationTimeMilliseconds;
	Runtime->Stats.bCheckpointInFlight = StorageStats.bCheckpointInFlight;
	Runtime->Stats.InterestCenter = Runtime->Observation.CurrentCenter;
	ClientReceiveStreamingStats(Runtime->Stats);
}

void UWorldChunkStreamingComponent::PumpServerOfferQueue()
{
	if (!Runtime || !Runtime->Server.bClientEndpointReady || Runtime->Server.PendingOffersToSend.IsEmpty())
	{
		return;
	}
	if (!CanSendReliablePayload(GetOwner(), MaximumOffersPerRpc * sizeof(FWorldChunkOffer)))
	{
		return;
	}
	struct FOfferCandidate final
	{
		FWorldChunkOffer Offer;
		double PriorityScore = TNumericLimits<double>::Max();
		bool bAlreadyPublished = false;
	};
	const auto IsHigherPriority = [](const FOfferCandidate& Left, const FOfferCandidate& Right)
	{
		if (Left.Offer.bActivationCore != Right.Offer.bActivationCore)
		{
			return Left.Offer.bActivationCore;
		}
		if (Left.bAlreadyPublished != Right.bAlreadyPublished)
		{
			// 超时重发不占新窗口，先完成已有协议事务。
			return Left.bAlreadyPublished;
		}
		return Left.PriorityScore != Right.PriorityScore ? Left.PriorityScore < Right.PriorityScore
														 : Left.Offer.Coord < Right.Offer.Coord;
	};
	TArray<FOfferCandidate> Candidates;
	Candidates.Reserve(MaximumOffersPerRpc);
	int32 PublishedOfferCount = 0;
	for (const TPair<FWorldChunkCoord, FServerChunkSubscription>& Pair : Runtime->Server.ServerSubscriptions)
	{
		PublishedOfferCount +=
			!Pair.Value.bSnapshotAcknowledged && Pair.Value.OfferFlow.OccupiesPublishedWindow() ? 1 : 0;
	}
	TArray<FWorldChunkCoord> InvalidCoords;
	for (const TPair<FWorldChunkCoord, FWorldChunkOffer>& Pair : Runtime->Server.PendingOffersToSend)
	{
		const FWorldChunkOffer& CandidateOffer = Pair.Value;
		if (!Runtime->Stats.bActivationCoreReady && !CandidateOffer.bActivationCore)
		{
			// 初次连接严格保留一条 Core-only 控制通道；外围 Offer 在核心双端
			// 基线就绪/ACK 后才开始，避免几千个可靠 Offer 把可玩门槛排在后面。
			continue;
		}
		const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(CandidateOffer.Coord);
		if (!Subscription || Subscription->bSnapshotAcknowledged ||
			Subscription->Offer.Revision != CandidateOffer.Revision ||
			Subscription->Offer.ContentHash != CandidateOffer.ContentHash)
		{
			InvalidCoords.Add(CandidateOffer.Coord);
			continue;
		}
		const bool bAlreadyPublished = Subscription->OfferFlow.OccupiesPublishedWindow();
		if (!FWorldChunkOfferFlowControl::CanPublish(bAlreadyPublished, PublishedOfferCount,
													 MaximumPublishedOffersInFlight))
		{
			continue;
		}

		FOfferCandidate Candidate;
		Candidate.Offer = CandidateOffer;
		Candidate.bAlreadyPublished = bAlreadyPublished;
		Candidate.PriorityScore = ChunkTransferPriorityScore(CandidateOffer.Coord, Runtime->Observation.CurrentLocation,
															 Runtime->Observation.CurrentForward);
		int32 Lower = 0;
		int32 Upper = Candidates.Num();
		while (Lower < Upper)
		{
			const int32 Middle = Lower + (Upper - Lower) / 2;
			if (IsHigherPriority(Candidates[Middle], Candidate))
			{
				Lower = Middle + 1;
			}
			else
			{
				Upper = Middle;
			}
		}
		if (Lower < MaximumOffersPerRpc)
		{
			Candidates.Insert(MoveTemp(Candidate), Lower);
			if (Candidates.Num() > MaximumOffersPerRpc)
			{
				Candidates.Pop(EAllowShrinking::No);
			}
		}
	}
	for (const FWorldChunkCoord& Coord : InvalidCoords)
	{
		Runtime->Server.PendingOffersToSend.Remove(Coord);
	}

	TArray<FWorldChunkOffer> Batch;
	Batch.Reserve(Candidates.Num());
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FOfferCandidate& Candidate : Candidates)
	{
		FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Candidate.Offer.Coord);
		if (!Subscription)
		{
			continue;
		}
		const bool bAlreadyPublished = Subscription->OfferFlow.OccupiesPublishedWindow();
		if (!FWorldChunkOfferFlowControl::CanPublish(bAlreadyPublished, PublishedOfferCount,
													 MaximumPublishedOffersInFlight))
		{
			continue;
		}
		Batch.Add(Candidate.Offer);
		Subscription->OfferFlow.MarkPublished();
		PublishedOfferCount += bAlreadyPublished ? 0 : 1;
		Subscription->LastOfferSentSeconds = NowSeconds;
		++Subscription->OfferSendCount;
		Runtime->Server.PendingOffersToSend.Remove(Candidate.Offer.Coord);
	}
	if (!Batch.IsEmpty())
	{
		if (!Runtime->Stats.bActivationCoreReady)
		{
			FString CoreCoords;
			for (const FWorldChunkOffer& Offer : Batch)
			{
				if (!Offer.bActivationCore)
				{
					continue;
				}
				const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Offer.Coord);
				CoreCoords += FString::Printf(TEXT(" (%d,%d,%d)#%d"), Offer.Coord.X, Offer.Coord.Y, Offer.Coord.Z,
											  Subscription ? Subscription->OfferSendCount : 0);
			}
			UE_LOG(LogTemp, Display, TEXT("Authority 发送 Activation Core Offer：%d 个；%s"), Batch.Num(), *CoreCoords);
		}
		// 每个网络帧最多一个 Reliable Offer RPC，避免大兴趣区首帧淹没 Reliable Buffer。
		ClientReceiveChunkOffers(Batch);
	}
}

void UWorldChunkStreamingComponent::ServerMarkChunkStreamingEndpointReady_Implementation()
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage || !Storage->IsAuthorityStorage())
	{
		return;
	}
	if (!Runtime->Server.bClientEndpointReady)
	{
		Runtime->Server.bClientEndpointReady = true;
		OfferAccumulator = OfferIntervalSeconds;
		UE_LOG(LogTemp, Display, TEXT("Client Chunk Streaming Endpoint Ready；开始发布 Authority Offer。"));
	}
}

void UWorldChunkStreamingComponent::ClientReceiveChunkOffers_Implementation(const TArray<FWorldChunkOffer>& Offers)
{
	if (!Runtime || !WorldStorage.IsValid() || Offers.Num() > MaximumOffersPerRpc)
	{
		return;
	}
	int32 CoreOfferCount = 0;
	int32 AcceptedCoreOfferCount = 0;
	for (const FWorldChunkOffer& Offer : Offers)
	{
		CoreOfferCount += Offer.bActivationCore ? 1 : 0;
		if (!Offer.WorldId.IsValid() || Offer.Revision == 0 || !Offer.ContentHash.IsSet() ||
			Offer.CompressedSize <= 0 || Offer.UncompressedSize <= 0)
		{
			UE_LOG(LogTemp, Warning,
				   TEXT("客户端拒绝 Chunk Offer (%d,%d,%d)：World=%d Revision=%u Hash=%d Compressed=%d Uncompressed=%d "
						"Core=%d。"),
				   Offer.Coord.X, Offer.Coord.Y, Offer.Coord.Z, Offer.WorldId.IsValid() ? 1 : 0, Offer.Revision,
				   Offer.ContentHash.IsSet() ? 1 : 0, Offer.CompressedSize, Offer.UncompressedSize,
				   Offer.bActivationCore ? 1 : 0);
			continue;
		}
		Runtime->Client.ClientOffersToCheck.Add(Offer.Coord, Offer);
		AcceptedCoreOfferCount += Offer.bActivationCore ? 1 : 0;
	}
	if (CoreOfferCount > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("客户端接收 Activation Core Offer：%d/%d 通过校验，待缓存查询=%d。"),
			   AcceptedCoreOfferCount, CoreOfferCount, Runtime->Client.ClientOffersToCheck.Num());
	}
}

void UWorldChunkStreamingComponent::PumpClientCacheLookups()
{
	if (!Runtime)
	{
		return;
	}
	TArray<FWorldChunkOffer> Candidates;
	for (const TPair<FWorldChunkCoord, FWorldChunkOffer>& Pair : Runtime->Client.ClientOffersToCheck)
	{
		if (!Runtime->Client.ClientCacheLookups.Contains(Pair.Key))
		{
			Candidates.Add(Pair.Value);
		}
	}
	Candidates.Sort(
		[Location = Runtime->Observation.CurrentLocation,
		 Forward = Runtime->Observation.CurrentForward](const FWorldChunkOffer& Left, const FWorldChunkOffer& Right)
		{
			const bool bLeftCore = Left.bActivationCore;
			const bool bRightCore = Right.bActivationCore;
			if (bLeftCore != bRightCore)
			{
				return bLeftCore;
			}
			const double LeftScore = ChunkTransferPriorityScore(Left.Coord, Location, Forward);
			const double RightScore = ChunkTransferPriorityScore(Right.Coord, Location, Forward);
			return LeftScore != RightScore ? LeftScore < RightScore : Left.Coord < Right.Coord;
		});

	for (const FWorldChunkOffer& Offer : Candidates)
	{
		// 上游未完成 Offer 窗口已约束快照数量；I/O 全部交给 UE Worker 队列，
		// 不再等前四个 Chunk 完成 GameThread 注入才允许读取后面的文件。
		Runtime->Client.ClientCacheLookups.Add(Offer.Coord);
		if (Offer.bActivationCore)
		{
			UE_LOG(LogTemp, Display, TEXT("Activation Core 缓存查询开始：(%d,%d,%d) Revision %u；在途=%d。"),
				   Offer.Coord.X, Offer.Coord.Y, Offer.Coord.Z, Offer.Revision,
				   Runtime->Client.ClientCacheLookups.Num());
		}
		const FString CacheRoot = GetClientCacheRoot(Offer.WorldId);
		const TWeakObjectPtr<UWorldChunkStreamingComponent> WeakThis(this);
		Async(EAsyncExecution::ThreadPool,
			  [WeakThis, CacheRoot, Offer]() mutable
			  {
				  FWorldCompressedChunk Cached;
				  const bool bFound = FWorldChunkClientCache::Load(CacheRoot, Offer, Cached);
				  if (!bFound)
				  {
					  // Cache 的 Revision + ContentHash 才是有效性门禁。旧文件在 Worker
					  // 查询确认 miss 后清理，不能把磁盘 I/O 回投到 GameThread。
					  IFileManager::Get().Delete(*FWorldChunkClientCache::MakeFilename(CacheRoot, Offer.Coord),
										 false, true);
				  }
				  AsyncTask(
					  ENamedThreads::GameThread,
					  [WeakThis, Offer, bFound, Cached = MoveTemp(Cached)]() mutable
					  {
						  UWorldChunkStreamingComponent* Component = WeakThis.Get();
						  if (!Component || !Component->Runtime)
						  {
							  return;
						  }
						  const FWorldChunkOffer* Current =
							  Component->Runtime->Client.ClientOffersToCheck.Find(Offer.Coord);
						  if (!Current || Current->Revision != Offer.Revision ||
							  Current->ContentHash != Offer.ContentHash)
						  {
							  Component->Runtime->Client.ClientCacheLookups.Remove(Offer.Coord);
							  return;
						  }
						  UWorldStorageSubsystem* Storage = Component->WorldStorage.Get();
						  const auto FinishLookup = [WeakThis, Offer](const bool bCacheHit)
						  {
							  UWorldChunkStreamingComponent* CurrentComponent = WeakThis.Get();
							  if (!CurrentComponent || !CurrentComponent->Runtime)
							  {
								  return;
							  }
							  CurrentComponent->Runtime->Client.ClientCacheLookups.Remove(Offer.Coord);
							  const FWorldChunkOffer* Latest =
								  CurrentComponent->Runtime->Client.ClientOffersToCheck.Find(Offer.Coord);
							  if (!Latest || Latest->Revision != Offer.Revision ||
								  Latest->ContentHash != Offer.ContentHash)
							  {
								  return;
							  }
							  FWorldChunkOfferResponse& Response =
								  CurrentComponent->Runtime->Client.CompletedClientResponses.AddDefaulted_GetRef();
							  Response.Coord = Offer.Coord;
							  Response.Revision = Offer.Revision;
							  Response.Response =
								  bCacheHit ? EWorldChunkClientResponse::Have : EWorldChunkClientResponse::Request;
							  if (Offer.bActivationCore)
							  {
								  UE_LOG(LogTemp, Display,
										 TEXT("Activation Core 缓存查询完成：(%d,%d,%d) Revision %u -> %s。"),
										 Offer.Coord.X, Offer.Coord.Y, Offer.Coord.Z, Offer.Revision,
										 bCacheHit ? TEXT("Have") : TEXT("Request"));
							  }
							  if (UWorldStorageSubsystem* CurrentStorage = CurrentComponent->WorldStorage.Get())
							  {
								  CurrentStorage->RecordClientCacheResult(bCacheHit);
							  }
							  CurrentComponent->Runtime->Client.ClientOffersToCheck.Remove(Offer.Coord);
						  };
						  if (!bFound || !Storage)
						  {
							  FinishLookup(false);
							  return;
						  }
						  const bool bAccepted = Storage->SubmitNetworkChunk(
							  Offer.WorldId, MoveTemp(Cached),
							  [FinishLookup](const bool bSuccess, const FString& Error, FWorldCompressedChunk&&)
							  {
								  if (!bSuccess && !Error.IsEmpty())
								  {
									  UE_LOG(LogTemp, Warning, TEXT("客户端缓存 Chunk 注入失败：%s"), *Error);
								  }
								  FinishLookup(bSuccess);
							  });
						  if (!bAccepted)
						  {
							  FinishLookup(false);
						  }
					  });
			  });
	}
	if (!Runtime->Client.CompletedClientResponses.IsEmpty())
	{
		const int32 Count = FMath::Min(MaximumOffersPerRpc, Runtime->Client.CompletedClientResponses.Num());
		TArray<FWorldChunkOfferResponse> Batch;
		Batch.Append(Runtime->Client.CompletedClientResponses.GetData(), Count);
		Runtime->Client.CompletedClientResponses.RemoveAt(0, Count, EAllowShrinking::No);
		ServerRespondToChunkOffers(Batch);
	}
}

void UWorldChunkStreamingComponent::ServerRespondToChunkOffers_Implementation(
	const TArray<FWorldChunkOfferResponse>& Responses)
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage || Responses.Num() > MaximumOffersPerRpc)
	{
		return;
	}
	for (const FWorldChunkOfferResponse& Response : Responses)
	{
		FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Response.Coord);
		if (!Subscription || Subscription->Offer.Revision != Response.Revision)
		{
			continue;
		}
		if (Response.Response != EWorldChunkClientResponse::Have &&
			Response.Response != EWorldChunkClientResponse::Request)
		{
			continue;
		}
		Subscription->OfferFlow.MarkClientResponseReceived();
		if (Response.Response == EWorldChunkClientResponse::Have)
		{
			Subscription->bSnapshotAcknowledged = true;
			Subscription->OfferFlow.ReleasePublishedWindow();
			Subscription->SnapshotFailureCount = 0;
			// 基线先封口；Delta 与统计由本帧统一 Pump，不能每个 ACK 全表扫描。
			continue;
		}
		if (Runtime->Server.OutgoingChunks.Contains(Response.Coord) ||
			Runtime->Server.SnapshotPreparations.Contains(Response.Coord))
		{
			continue;
		}
		Runtime->Server.PendingSnapshotRequests.AddUnique(Response.Coord);
	}
}

void UWorldChunkStreamingComponent::PumpServerSnapshotPreparations()
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage)
	{
		return;
	}
	TArray<FWorldChunkCoord> Requests = MoveTemp(Runtime->Server.PendingSnapshotRequests);
	Requests.Sort(
		[Center = Runtime->Observation.CurrentCenter, Location = Runtime->Observation.CurrentLocation,
		 Forward = Runtime->Observation.CurrentForward](const FWorldChunkCoord& Left, const FWorldChunkCoord& Right)
		{
			const bool bLeftCore = IsActivationCore(Left, Center);
			const bool bRightCore = IsActivationCore(Right, Center);
			if (bLeftCore != bRightCore) return bLeftCore;
			const double LeftScore = ChunkTransferPriorityScore(Left, Location, Forward);
			const double RightScore = ChunkTransferPriorityScore(Right, Location, Forward);
			return LeftScore != RightScore ? LeftScore < RightScore : Left < Right;
		});
	// 未完成 Offer 已给出有界工作集；一次排序后全部提交，Worker 完成不依赖下一帧释放两份名额。
	for (const FWorldChunkCoord Coord : Requests)
	{
		const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
		if (!Subscription || Subscription->bSnapshotAcknowledged || Runtime->Server.OutgoingChunks.Contains(Coord)
			|| Runtime->Server.SnapshotPreparations.Contains(Coord))
		{
			continue;
		}
		const FWorldChunkOffer ExpectedOffer = Subscription->Offer;
		Runtime->Server.SnapshotPreparations.Add(Coord);
		const TWeakObjectPtr<UWorldChunkStreamingComponent> WeakThis(this);
		if (!Storage->RequestCurrentCompressedChunk(
				Coord,
				[WeakThis, ExpectedOffer](FWorldCompressedChunk&& Compressed, FString&& Error)
				{
					UWorldChunkStreamingComponent* Component = WeakThis.Get();
					if (!Component || !Component->Runtime)
					{
						return;
					}
					Component->Runtime->Server.SnapshotPreparations.Remove(ExpectedOffer.Coord);
					FServerChunkSubscription* Current =
						Component->Runtime->Server.ServerSubscriptions.Find(ExpectedOffer.Coord);
					if (!Current || !Error.IsEmpty() || !Compressed.IsValid() ||
						Current->Offer.Revision != ExpectedOffer.Revision ||
						Current->Offer.ContentHash != ExpectedOffer.ContentHash ||
						Compressed.Revision != ExpectedOffer.Revision ||
						Compressed.ContentHash != ExpectedOffer.ContentHash)
					{
						if (Current && !Current->bSnapshotAcknowledged)
						{
							// Snapshot 已在异步准备期间失效时，等待下一次 Offer 发布新 Revision；
							// 继续请求旧 Revision 只会形成逐帧重试循环。
							Component->OfferAccumulator = OfferIntervalSeconds;
						}
						return;
					}
					FOutgoingChunk Outgoing;
					Outgoing.Offer = ExpectedOffer;
					Outgoing.Bytes = MoveTemp(Compressed.Bytes);
					uint64 TransferId = Component->Runtime->Server.NextBulkTransferId++;
					if (TransferId == 0)
					{
						TransferId = Component->Runtime->Server.NextBulkTransferId++;
					}
					Outgoing.BulkPayloadId = {
						WorldStorageTransferDomain,
						TransferId,
						ExpectedOffer.Revision};
					Component->Runtime->Server.OutgoingChunks.Add(ExpectedOffer.Coord, MoveTemp(Outgoing));
				}))
		{
			Runtime->Server.SnapshotPreparations.Remove(Coord);
		}
	}
}

void UWorldChunkStreamingComponent::PumpServerPayload()
{
	if (!Runtime)
	{
		return;
	}
	PumpServerSnapshotPreparations();
	RefreshActivationCoreReadiness();
	if (!BulkTransferScheduler)
	{
		return;
	}
	TArray<FWorldChunkCoord> Coords;
	Runtime->Server.OutgoingChunks.GenerateKeyArray(Coords);
	Coords.Sort(
		[Center = Runtime->Observation.CurrentCenter, Location = Runtime->Observation.CurrentLocation,
		 Forward = Runtime->Observation.CurrentForward](const FWorldChunkCoord& Left, const FWorldChunkCoord& Right)
		{
			const bool bLeftCore = IsActivationCore(Left, Center);
			const bool bRightCore = IsActivationCore(Right, Center);
			if (bLeftCore != bRightCore)
			{
				return bLeftCore;
			}
			const double LeftScore = ChunkTransferPriorityScore(Left, Location, Forward);
			const double RightScore = ChunkTransferPriorityScore(Right, Location, Forward);
			return LeftScore != RightScore ? LeftScore < RightScore : Left < Right;
		});
	for (const FWorldChunkCoord& Coord : Coords)
	{
		FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Coord);
		if (!Outgoing || Outgoing->bQueuedToBulkScheduler || Outgoing->Bytes.IsEmpty()
			|| !Outgoing->BulkPayloadId.IsValid())
		{
			continue;
		}
		const FWorldChunkOffer Offer = Outgoing->Offer;
		const FPayloadId PayloadId = Outgoing->BulkPayloadId;
		const TWeakObjectPtr<UWorldChunkStreamingComponent> WeakThis(this);
		const bool bQueued = BulkTransferScheduler->Enqueue(
			ETransferClass::WorldStorage,
			PayloadId,
			Outgoing->Bytes,
			SegmentPayloadBytes,
			[WeakThis, Offer](FSegment&& BulkSegment)
			{
				UWorldChunkStreamingComponent* Component = WeakThis.Get();
				if (!Component || !Component->Runtime)
				{
					return;
				}
				FWorldChunkPayloadSegment Segment;
				Segment.Offer = Offer;
				Segment.SegmentIndex = BulkSegment.SegmentIndex;
				Segment.SegmentCount = BulkSegment.SegmentCount;
				Segment.Bytes = MoveTemp(BulkSegment.Bytes);
				Component->Runtime->Stats.PayloadBytesSent += Segment.Bytes.Num();
				Component->ClientReceiveChunkSegment(Segment);
			});
		if (bQueued)
		{
			Outgoing->bQueuedToBulkScheduler = true;
			Outgoing->Bytes.Reset();
		}
	}
	while (BulkTransferScheduler->TryDispatch())
	{
	}
	Runtime->Stats.SegmentsInFlight = BulkTransferScheduler->GetInFlightCount();
	Runtime->Stats.PendingChunkCount =
		Runtime->Server.PendingOffersToSend.Num() + Runtime->Server.OutgoingChunks.Num() +
		Runtime->Server.PendingSnapshotRequests.Num() + Runtime->Server.SnapshotPreparations.Num();
	if (Runtime->Stats.bActivationCoreReady && !Runtime->Activation.bActivationCoreReadyLogged)
	{
		Runtime->Activation.bActivationCoreReadyLogged = true;
		Runtime->Stats.ActivationCoreReadySeconds = FPlatformTime::Seconds() - Runtime->BeginPlaySeconds;
		UE_LOG(LogTemp, Display, TEXT("Chunk Activation Core 已就绪：%.3f 秒；ACK %d/%d。"),
			   Runtime->Stats.ActivationCoreReadySeconds, Runtime->Stats.AcknowledgedChunkCount,
			   Runtime->Stats.OfferedChunkCount);
	}
	const double NowSeconds = FPlatformTime::Seconds();
	if (!Runtime->Stats.bActivationCoreReady &&
		NowSeconds - Runtime->Activation.LastActivationCoreProgressLogSeconds >= 2.0)
	{
		Runtime->Activation.LastActivationCoreProgressLogSeconds = NowSeconds;
			UWorldStorageSubsystem* Storage = WorldStorage.Get();
			FWorldChunkCoord FirstMissing;
			bool bFoundMissing = false;
			bool bFirstAcknowledged = false;
			bool bFirstAuthorityReady = false;
			bool bFirstDirty = false;
			int32 FirstResidentEntities = 0;
			for (const FWorldChunkCoord& Coord : Runtime->Server.ActivationCoreChunks)
			{
				const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
				const bool bAcknowledged = Subscription && Subscription->bSnapshotAcknowledged;
				const bool bAuthorityReady = Storage && Storage->IsAuthorityChunkReadyForActivation(Coord);
				if (!bAcknowledged || !bAuthorityReady)
				{
					FirstMissing = Coord;
					bFoundMissing = true;
					bFirstAcknowledged = bAcknowledged;
					bFirstAuthorityReady = bAuthorityReady;
					bFirstDirty = Storage && Storage->IsChunkDirty(Coord);
					FirstResidentEntities = Storage ? Storage->GetChunkResidentEntityCount(Coord) : 0;
					break;
				}
			}
			UE_LOG(LogTemp, Display,
				   TEXT("Activation Core 进度：Center=(%d,%d,%d)，C ACK=%d/%d，S Ready=%d/%d；")
					   TEXT("Offer=%d Outgoing=%d Request=%d Prepare=%d；首个缺口=(%d,%d,%d) ack=%d ready=%d dirty=%d "
							"entities=%d。"),
				   Runtime->Observation.CurrentCenter.X, Runtime->Observation.CurrentCenter.Y,
				   Runtime->Observation.CurrentCenter.Z, Runtime->Stats.ActivationCoreAcknowledgedChunkCount,
				   Runtime->Stats.ActivationCoreChunkCount, Runtime->Stats.ActivationCoreAuthorityReadyChunkCount,
				   Runtime->Stats.ActivationCoreChunkCount, Runtime->Server.PendingOffersToSend.Num(),
				   Runtime->Server.OutgoingChunks.Num(), Runtime->Server.PendingSnapshotRequests.Num(),
				   Runtime->Server.SnapshotPreparations.Num(), bFoundMissing ? FirstMissing.X : 0,
				   bFoundMissing ? FirstMissing.Y : 0, bFoundMissing ? FirstMissing.Z : 0, bFirstAcknowledged ? 1 : 0,
				   bFirstAuthorityReady ? 1 : 0, bFirstDirty ? 1 : 0, FirstResidentEntities);
		}
}

void UWorldChunkStreamingComponent::ClientReceiveChunkSegment_Implementation(const FWorldChunkPayloadSegment& Segment)
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	FString RejectionReason;
	if (!Runtime || !Storage)
	{
		RejectionReason = TEXT("组件或 WorldStorage 已失效");
	}
	else if (!Segment.Offer.WorldId.IsValid() || Segment.Offer.Revision == 0 || !Segment.Offer.ContentHash.IsSet() ||
			 Segment.Offer.CompressedSize <= 0 || Segment.Offer.UncompressedSize <= 0)
	{
		RejectionReason = TEXT("Offer 元数据无效");
	}
	else if (Segment.SegmentCount <= 0 ||
			 Segment.SegmentCount != FMath::DivideAndRoundUp(Segment.Offer.CompressedSize, SegmentPayloadBytes))
	{
		RejectionReason = TEXT("分段总数与压缩大小不一致");
	}
	else if (Segment.SegmentIndex < 0 || Segment.SegmentIndex >= Segment.SegmentCount)
	{
		RejectionReason = TEXT("分段序号越界");
	}
	else if (Segment.Bytes.IsEmpty() || Segment.Bytes.Num() > SegmentPayloadBytes)
	{
		RejectionReason = TEXT("分段 Payload 大小无效");
	}
	if (!RejectionReason.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			   TEXT("客户端拒绝 Chunk (%d,%d,%d) Revision %u 的 Segment %d/%d：%s；Offer=%d bytes，Payload=%d bytes。"),
			   Segment.Offer.Coord.X, Segment.Offer.Coord.Y, Segment.Offer.Coord.Z, Segment.Offer.Revision,
			   Segment.SegmentIndex, Segment.SegmentCount, *RejectionReason, Segment.Offer.CompressedSize,
			   Segment.Bytes.Num());
		if (Runtime && Segment.Offer.Revision != 0)
		{
			ServerRejectChunkSnapshot(Segment.Offer.Coord, Segment.Offer.Revision);
		}
		return;
	}
	FWorldChunkSegmentAssembly& Incoming = Runtime->Client.IncomingChunks.FindOrAdd(Segment.Offer.Coord);
	const EWorldChunkSegmentAcceptResult AcceptResult = Incoming.Accept(Segment, SegmentPayloadBytes);
	if (AcceptResult == EWorldChunkSegmentAcceptResult::Rejected)
	{
		Runtime->Client.IncomingChunks.Remove(Segment.Offer.Coord);
		ServerRejectChunkSnapshot(Segment.Offer.Coord, Segment.Offer.Revision);
		return;
	}
	if (AcceptResult != EWorldChunkSegmentAcceptResult::Duplicate)
	{
		Runtime->Stats.PayloadBytesReceived += Segment.Bytes.Num();
	}
	ServerAcknowledgeChunkSegment(Segment.Offer.Coord, Segment.Offer.Revision, Segment.SegmentIndex);
	if (AcceptResult != EWorldChunkSegmentAcceptResult::Completed)
	{
		return;
	}
	FWorldCompressedChunk Compressed;
	if (!Incoming.Build(Compressed))
	{
		Runtime->Client.IncomingChunks.Remove(Segment.Offer.Coord);
		ServerRejectChunkSnapshot(Segment.Offer.Coord, Segment.Offer.Revision);
		return;
	}
	const FWorldChunkOffer CompletedOffer = Incoming.GetOffer();
	Runtime->Client.IncomingChunks.Remove(Segment.Offer.Coord);
	const TWeakObjectPtr<UWorldChunkStreamingComponent> WeakThis(this);
	if (!Storage->SubmitNetworkChunk(
			CompletedOffer.WorldId, MoveTemp(Compressed),
			[WeakThis, CompletedOffer](const bool bSuccess, const FString& Error,
									   FWorldCompressedChunk&& CacheChunk) mutable
			{
				UWorldChunkStreamingComponent* Component = WeakThis.Get();
				if (!Component || !Component->Runtime)
				{
					return;
				}
				if (!bSuccess)
				{
					UE_LOG(LogTemp, Error, TEXT("网络 Chunk 注入失败：%s"), *Error);
					Component->ServerRejectChunkSnapshot(CompletedOffer.Coord, CompletedOffer.Revision);
					return;
				}
				const FString CacheRoot = Component->GetClientCacheRoot(CompletedOffer.WorldId);
				Async(EAsyncExecution::ThreadPool,
					  [CacheRoot, WorldId = CompletedOffer.WorldId, CacheChunk = MoveTemp(CacheChunk)]() mutable
					  { FWorldChunkClientCache::Save(CacheRoot, WorldId, CacheChunk); });
				Component->ServerAcknowledgeChunk(CompletedOffer.Coord, CompletedOffer.Revision);
			}))
	{
		ServerRejectChunkSnapshot(CompletedOffer.Coord, CompletedOffer.Revision);
	}
}

void UWorldChunkStreamingComponent::ServerAcknowledgeChunkSegment_Implementation(const FWorldChunkCoord Coord,
																 const uint32 Revision,
																 const int32 SegmentIndex)
{
	FOutgoingChunk* Outgoing = Runtime ? Runtime->Server.OutgoingChunks.Find(Coord) : nullptr;
	if (Outgoing && Outgoing->Offer.Revision == Revision && BulkTransferScheduler
		&& SegmentIndex >= 0 && SegmentIndex <= MAX_uint16)
	{
		BulkTransferScheduler->Acknowledge(
			Outgoing->BulkPayloadId,
			static_cast<uint16>(SegmentIndex));
	}
}

void UWorldChunkStreamingComponent::ServerAcknowledgeChunk_Implementation(const FWorldChunkCoord Coord,
																		  const uint32 Revision)
{
	if (!Runtime)
	{
		return;
	}
	FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
	if (!Subscription || Subscription->Offer.Revision != Revision)
	{
		return;
	}
	Subscription->bSnapshotAcknowledged = true;
	Subscription->OfferFlow.ReleasePublishedWindow();
	Subscription->SnapshotFailureCount = 0;
	if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Coord);
		Outgoing && BulkTransferScheduler)
	{
		BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
	}
	Runtime->Server.OutgoingChunks.Remove(Coord);
	// 注入 ACK 只提交本 Chunk 状态；本帧统一发送 Delta、刷新统计并推进其余快照。
}

void UWorldChunkStreamingComponent::ServerRejectChunkSnapshot_Implementation(const FWorldChunkCoord Coord,
																			 const uint32 Revision)
{
	FServerChunkSubscription* Subscription = Runtime ? Runtime->Server.ServerSubscriptions.Find(Coord) : nullptr;
	if (!Subscription || Subscription->Offer.Revision != Revision || Subscription->bSnapshotAcknowledged)
	{
		return;
	}
	if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Coord);
		Outgoing && BulkTransferScheduler)
	{
		BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
	}
	Runtime->Server.OutgoingChunks.Remove(Coord);
	Runtime->Server.SnapshotPreparations.Remove(Coord);
	++Subscription->SnapshotFailureCount;
	if (Subscription->SnapshotFailureCount <= 3)
	{
		Runtime->Server.PendingSnapshotRequests.AddUnique(Coord);
	}
	else
	{
		Subscription->OfferFlow.ReleasePublishedWindow();
		UE_LOG(LogTemp, Error, TEXT("Chunk (%d,%d,%d) Revision %u 连续三次被 Client 拒绝，停止本次 Snapshot 重试。"),
			   Coord.X, Coord.Y, Coord.Z, Revision);
	}
	RefreshServerStreamingStats();
}

void UWorldChunkStreamingComponent::HandleAuthorityMutation(const FWorldStorageEntityMutation& Mutation)
{
	if (!Runtime || !Mutation.EntityId.IsSet() || Mutation.StateRevision == 0)
	{
		return;
	}
	const bool bSubscribedBefore = Runtime->Server.ServerSubscriptions.Contains(Mutation.PreviousChunk);
	const bool bSubscribedNow = Runtime->Server.ServerSubscriptions.Contains(Mutation.CurrentChunk);
	const auto QueueDelta = [this, &Mutation](const FWorldChunkCoord& Coord, const EWorldChunkLiveDeltaKind Kind,
											  const FWorldPersistentEntityRecord* Record)
	{
		FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
		if (!Subscription)
		{
			return;
		}
		FWorldChunkLiveDelta Delta;
		Delta.Kind = Kind;
		Delta.ChunkCoord = Coord;
		Delta.EntityId = Mutation.EntityId;
		Delta.StateRevision = Mutation.StateRevision;
		if (Record)
		{
			Delta.Record = *Record;
		}
		Runtime->Server.LiveDeltaQueue.Enqueue(MoveTemp(Delta));
	};

	if (Mutation.Kind == EWorldStorageMutationKind::GameplayTombstone)
	{
		if (bSubscribedBefore)
		{
			QueueDelta(Mutation.PreviousChunk, EWorldChunkLiveDeltaKind::GameplayTombstone, nullptr);
		}
		return;
	}
	if (!bSubscribedNow && Mutation.Kind == EWorldStorageMutationKind::Move && bSubscribedBefore)
	{
		QueueDelta(Mutation.PreviousChunk, EWorldChunkLiveDeltaKind::ProjectionRemove, nullptr);
		return;
	}
	if (!bSubscribedNow)
	{
		return;
	}
	FWorldPersistentEntityRecord Record;
	FString Error;
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Storage || !Storage->CaptureResidentRecord(Mutation.EntityId, Record, Error))
	{
		return;
	}
	if (Record.Payload.Num() > MaximumLiveDeltaPayloadBytes)
	{
		FServerChunkSubscription& Subscription = Runtime->Server.ServerSubscriptions.FindChecked(Mutation.CurrentChunk);
		Subscription.bSnapshotAcknowledged = false;
		if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Mutation.CurrentChunk);
			Outgoing && BulkTransferScheduler)
		{
			BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
		}
		Runtime->Server.OutgoingChunks.Remove(Mutation.CurrentChunk);
		OfferAccumulator = OfferIntervalSeconds;
		return;
	}
	QueueDelta(Mutation.CurrentChunk, EWorldChunkLiveDeltaKind::Upsert, &Record);
	UE_LOG(LogElementSandboxWorldStorage, Verbose,
		TEXT("Live Delta 已排队：Entity=%llu Chunk=(%d,%d,%d) Revision=%u。"),
		Mutation.EntityId.GetValue(), Mutation.CurrentChunk.X, Mutation.CurrentChunk.Y,
		Mutation.CurrentChunk.Z, Mutation.StateRevision);
}

void UWorldChunkStreamingComponent::PumpServerLiveDeltas()
{
	if (!Runtime || !Runtime->Server.bClientEndpointReady || !Runtime->Server.LiveDeltaFlow.CanPublish()
		|| Runtime->Server.LiveDeltaQueue.IsEmpty())
	{
		return;
	}
	// 依据真实剩余额度装批，不能因完整 96 KiB 放不下就连脚边的一条状态也不发送。
	const int32 AvailableBytes = GetReliablePayloadBudget(GetOwner(), MaximumLiveDeltaBatchBytes);
	if (AvailableBytes <= 0) return;

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	const FVector Location = Pawn ? Pawn->GetActorLocation() : Runtime->Observation.CurrentLocation;
	TArray<FWorldChunkLiveDelta> Batch;
	if (!Runtime->Server.LiveDeltaQueue.BuildBatch(
		Location,
		[this](const FWorldChunkCoord Coord)
		{
			const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord);
			return Subscription && Subscription->bSnapshotAcknowledged;
		},
		MaximumLiveDeltasPerBatch, AvailableBytes, Batch))
	{
		return;
	}

	const uint64 Sequence = Runtime->Server.LiveDeltaFlow.BeginBatch();
	if (!ensure(Sequence != 0))
	{
		return;
	}
	TSet<FWorldChunkCoord> BatchCoords;
	for (const FWorldChunkLiveDelta& Delta : Batch) BatchCoords.Add(Delta.ChunkCoord);
	Runtime->Server.LiveDeltaBatchCoordsInFlight.Add(Sequence, MoveTemp(BatchCoords));
	ClientReceiveLiveDeltaBatch(Sequence, Batch);
}

void UWorldChunkStreamingComponent::ClientReceiveLiveDeltaBatch_Implementation(
	const uint64 Sequence, const TArray<FWorldChunkLiveDelta>& Deltas)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ClientQueueLiveDeltaBatch);
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Runtime || !Storage || Sequence == 0 || Deltas.IsEmpty() || Deltas.Num() > MaximumLiveDeltasPerBatch)
	{
		return;
	}
	int32 EstimatedBytes = 0;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ClientValidateLiveDeltaBatch);
		for (const FWorldChunkLiveDelta& Delta : Deltas)
		{
			EstimatedBytes += 256 + Delta.Record.Payload.Num();
			const bool bValid =
				Delta.EntityId.IsSet() && Delta.StateRevision != 0 &&
				(Delta.Kind != EWorldChunkLiveDeltaKind::Upsert ||
				 (Delta.Record.EntityId == Delta.EntityId && Delta.Record.StateRevision == Delta.StateRevision &&
				  Delta.Record.Payload.Num() <= MaximumLiveDeltaPayloadBytes));
			if (!bValid || EstimatedBytes > MaximumLiveDeltaBatchBytes)
			{
				ServerAcknowledgeLiveDeltaBatch(Sequence, false);
				return;
			}
		}
	}
	TArray<FWorldChunkLiveDelta> QueuedDeltas = Deltas;
	switch (Runtime->Client.LiveDeltaQueue.Enqueue(Sequence, MoveTemp(QueuedDeltas)))
	{
	case EWorldChunkLiveDeltaEnqueueResult::Queued:
	case EWorldChunkLiveDeltaEnqueueResult::AlreadyQueued:
		return;
	case EWorldChunkLiveDeltaEnqueueResult::AlreadyApplied:
		ServerAcknowledgeLiveDeltaBatch(Sequence, true);
		return;
	case EWorldChunkLiveDeltaEnqueueResult::OutOfOrder:
	case EWorldChunkLiveDeltaEnqueueResult::Full:
		ServerAcknowledgeLiveDeltaBatch(Sequence, false);
		return;
	}
}

void UWorldChunkStreamingComponent::PumpClientLiveDeltaBatch()
{
	if (!Runtime)
	{
		return;
	}
	const FQueuedWorldChunkLiveDeltaBatch* Batch = Runtime->Client.LiveDeltaQueue.Peek();
	if (!Batch)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldStorage_ClientApplyOneLiveDeltaBatch);
	const uint64 Sequence = Batch->Sequence;
	const int32 DeltaCount = Batch->Deltas.Num();
	const bool bApplied = ApplyClientLiveDeltaBatch(Batch->Deltas);
	if (bApplied)
	{
		verify(Runtime->Client.LiveDeltaQueue.CompleteFront(Sequence));
		UE_LOG(LogElementSandboxWorldStorage, Verbose,
			TEXT("Client 已应用 Live Delta Batch：Sequence=%llu Deltas=%d。"), Sequence, DeltaCount);
		ServerAcknowledgeLiveDeltaBatch(Sequence, true);
		return;
	}

	// 任一批次失败后必须丢弃其后的预取批次并逐个 NACK，服务器才能释放双窗口，
	// 再以完整 Snapshot 重建受影响 Chunk。
	TArray<uint64> FailedSequences;
	Runtime->Client.LiveDeltaQueue.DiscardAllAsConsumed(FailedSequences);
	for (const uint64 FailedSequence : FailedSequences)
	{
		ServerAcknowledgeLiveDeltaBatch(FailedSequence, false);
	}
}

bool UWorldChunkStreamingComponent::ApplyClientLiveDeltaBatch(
	const TConstArrayView<FWorldChunkLiveDelta> Deltas)
{
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	if (!Storage || Deltas.IsEmpty())
	{
		return false;
	}
	const bool bAllRemovals = Algo::AllOf(Deltas, [](const FWorldChunkLiveDelta& Delta)
	{
		return Delta.Kind != EWorldChunkLiveDeltaKind::Upsert;
	});
	if (bAllRemovals)
	{
		TArray<FWorldNetworkEntityRemoval, TInlineAllocator<256>> Removals;
		Removals.Reserve(Deltas.Num());
		for (const FWorldChunkLiveDelta& Delta : Deltas)
		{
			Removals.Add({Delta.EntityId, Delta.StateRevision,
				Delta.Kind == EWorldChunkLiveDeltaKind::GameplayTombstone});
		}
		return Storage->ApplyNetworkRemoveBatch(Removals);
	}

	TArray<FWorldPersistentEntityRecord, TInlineAllocator<256>> Records;
	Records.Reserve(Deltas.Num());
	for (const FWorldChunkLiveDelta& Delta : Deltas)
	{
		if (Delta.Kind != EWorldChunkLiveDeltaKind::Upsert)
		{
			return false;
		}
		Records.Add(Delta.Record);
	}
	return Storage->ApplyNetworkUpsertBatch(Records);
}

void UWorldChunkStreamingComponent::ServerAcknowledgeLiveDeltaBatch_Implementation(const uint64 Sequence,
																				   const bool bApplied)
{
	if (!Runtime || !Runtime->Server.LiveDeltaFlow.Acknowledge(Sequence))
	{
		return;
	}
	TSet<FWorldChunkCoord> BatchCoords;
	if (TSet<FWorldChunkCoord>* Found = Runtime->Server.LiveDeltaBatchCoordsInFlight.Find(Sequence))
	{
		BatchCoords = MoveTemp(*Found);
	}
	Runtime->Server.LiveDeltaBatchCoordsInFlight.Remove(Sequence);
	if (!bApplied)
	{
		for (const FWorldChunkCoord& Coord : BatchCoords)
		{
			if (FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(Coord))
			{
				Subscription->bSnapshotAcknowledged = false;
				Runtime->Server.LiveDeltaQueue.RemoveChunk(Coord);
				Subscription->OfferFlow.Reset();
				Subscription->LastOfferSentSeconds = -DBL_MAX;
				if (FOutgoingChunk* Outgoing = Runtime->Server.OutgoingChunks.Find(Coord);
					Outgoing && BulkTransferScheduler)
				{
					BulkTransferScheduler->Cancel(Outgoing->BulkPayloadId);
				}
				Runtime->Server.OutgoingChunks.Remove(Coord);
				Runtime->Server.SnapshotPreparations.Remove(Coord);
				Runtime->Server.PendingSnapshotRequests.Remove(Coord);
				Runtime->Server.PendingOffersToSend.Add(Coord, Subscription->Offer);
			}
		}
		OfferAccumulator = OfferIntervalSeconds;
	}
	PumpServerLiveDeltas();
}

void UWorldChunkStreamingComponent::ClientReceiveStreamingStats_Implementation(
	const FWorldChunkStreamingStats AuthorityStats)
{
	if (!Runtime)
	{
		return;
	}
	const int64 LocalPayloadBytesReceived = Runtime->Stats.PayloadBytesReceived;
	const bool bWasInitialActivationReady = Runtime->Stats.bActivationCoreReady;
	Runtime->Stats = AuthorityStats;
	Runtime->Stats.PayloadBytesReceived = LocalPayloadBytesReceived;
	Runtime->Stats.bActivationCoreReady = IsInitialActivationGateSatisfied(
		bWasInitialActivationReady, AuthorityStats.bActivationCoreReady);
	ApplyClientActivationGate(Runtime->Stats.bActivationCoreReady);
}

void UWorldChunkStreamingComponent::RefreshServerStreamingStats()
{
	if (!Runtime)
	{
		return;
	}
	Runtime->Stats.OfferedChunkCount = Runtime->Server.ServerSubscriptions.Num();
	Runtime->Stats.AcknowledgedChunkCount = 0;
	for (const TPair<FWorldChunkCoord, FServerChunkSubscription>& Pair : Runtime->Server.ServerSubscriptions)
	{
		Runtime->Stats.AcknowledgedChunkCount += Pair.Value.bSnapshotAcknowledged ? 1 : 0;
	}
	RefreshActivationCoreReadiness();
	Runtime->Stats.PendingChunkCount =
		Runtime->Server.PendingOffersToSend.Num() + Runtime->Server.OutgoingChunks.Num() +
		Runtime->Server.PendingSnapshotRequests.Num() + Runtime->Server.SnapshotPreparations.Num();
	Runtime->Stats.PendingLiveDeltaCount = Runtime->Server.LiveDeltaQueue.Num();
}

void UWorldChunkStreamingComponent::RefreshActivationCoreReadiness()
{
	if (!Runtime)
	{
		return;
	}
	const bool bWasInitialActivationReady = Runtime->Activation.bInitialActivationGateSatisfied;
	Runtime->Stats.ActivationCoreChunkCount = Runtime->Server.ActivationCoreChunks.Num();
	Runtime->Stats.ActivationCoreAcknowledgedChunkCount = 0;
	Runtime->Stats.ActivationCoreAuthorityReadyChunkCount = 0;
	UWorldStorageSubsystem* Storage = WorldStorage.Get();
	for (const FWorldChunkCoord& CoreCoord : Runtime->Server.ActivationCoreChunks)
	{
		const FServerChunkSubscription* Subscription = Runtime->Server.ServerSubscriptions.Find(CoreCoord);
		Runtime->Stats.ActivationCoreAcknowledgedChunkCount +=
			Subscription && Subscription->bSnapshotAcknowledged ? 1 : 0;
		Runtime->Stats.ActivationCoreAuthorityReadyChunkCount +=
			Storage && Storage->IsAuthorityChunkReadyForActivation(CoreCoord) ? 1 : 0;
	}
	const bool bCurrentCoreComplete = IsActivationCoreGateComplete(
		Storage && Storage->IsStorageReady(), Runtime->Server.bClientEndpointReady,
		Runtime->Stats.ActivationCoreChunkCount, Runtime->Stats.ActivationCoreAcknowledgedChunkCount,
		Runtime->Stats.ActivationCoreAuthorityReadyChunkCount);
	Runtime->Activation.bInitialActivationGateSatisfied = IsInitialActivationGateSatisfied(
		Runtime->Activation.bInitialActivationGateSatisfied, bCurrentCoreComplete);
	Runtime->Stats.bActivationCoreReady = Runtime->Activation.bInitialActivationGateSatisfied;
	ApplyAuthorityActivationGate(Runtime->Activation.bInitialActivationGateSatisfied);
	if (!bWasInitialActivationReady && Runtime->Activation.bInitialActivationGateSatisfied)
	{
		// 放行信号不等待下一次 1 秒 HUD 统计推送。
		ClientReceiveStreamingStats(Runtime->Stats);
	}
}

void UWorldChunkStreamingComponent::ApplyAuthorityActivationGate(const bool bReady)
{
	if (!Runtime)
	{
		return;
	}
	const auto ReleaseStoredCharacter = [this]()
	{
		if (!Runtime->Activation.bAuthorityMovementBlockedForActivationCore)
		{
			return;
		}
		if (ACharacter* Character = Runtime->Activation.AuthorityGatedCharacter.Get())
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->SetMovementMode(static_cast<EMovementMode>(Runtime->Activation.AuthorityPreviousMovementMode),
										  Runtime->Activation.AuthorityPreviousCustomMovementMode);
			}
		}
		Runtime->Activation.AuthorityGatedCharacter.Reset();
		Runtime->Activation.bAuthorityMovementBlockedForActivationCore = false;
	};

	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	ACharacter* CurrentCharacter = PlayerController ? Cast<ACharacter>(PlayerController->GetPawn()) : nullptr;
	if (bReady || !CurrentCharacter)
	{
		ReleaseStoredCharacter();
		return;
	}
	if (Runtime->Activation.bAuthorityMovementBlockedForActivationCore &&
		Runtime->Activation.AuthorityGatedCharacter.Get() == CurrentCharacter)
	{
		return;
	}
	ReleaseStoredCharacter();
	UCharacterMovementComponent* Movement = CurrentCharacter->GetCharacterMovement();
	if (!Movement)
	{
		return;
	}
	Runtime->Activation.AuthorityPreviousMovementMode = static_cast<uint8>(Movement->MovementMode);
	Runtime->Activation.AuthorityPreviousCustomMovementMode = Movement->CustomMovementMode;
	Runtime->Activation.AuthorityGatedCharacter = CurrentCharacter;
	Runtime->Activation.bAuthorityMovementBlockedForActivationCore = true;
	Movement->StopMovementImmediately();
	Movement->DisableMovement();
}

void UWorldChunkStreamingComponent::ApplyClientActivationGate(const bool bReady)
{
	if (!Runtime)
	{
		return;
	}
	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return;
	}
	const bool bShouldBlock = !bReady;
	if (bClientInputBlockedForActivationCore == bShouldBlock)
	{
		return;
	}
	PlayerController->SetIgnoreMoveInput(bShouldBlock);
	PlayerController->SetIgnoreLookInput(bShouldBlock);
	bClientInputBlockedForActivationCore = bShouldBlock;
	UE_LOG(LogTemp, Display, TEXT("Activation Core 客户端输入门禁%s；MoveIgnored=%d LookIgnored=%d。"),
		   bShouldBlock ? TEXT("已锁定") : TEXT("已释放"), PlayerController->IsMoveInputIgnored(),
		   PlayerController->IsLookInputIgnored());
}

FString UWorldChunkStreamingComponent::GetClientCacheRoot(const FGuid& WorldId) const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldChunkCache"),
						   Runtime ? Runtime->Client.ServerFingerprint : TEXT("UnknownServer"),
						   WorldId.ToString(EGuidFormats::Digits));
}
