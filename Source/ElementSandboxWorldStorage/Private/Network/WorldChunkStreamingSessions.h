#pragma once

#include "Network/WorldChunkClientCache.h"
#include "Network/WorldChunkClientLiveDeltaQueue.h"
#include "Network/WorldChunkLiveDeltaFlowControl.h"
#include "Network/WorldChunkOfferFlowControl.h"
#include "Network/WorldChunkServerLiveDeltaQueue.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "NetBulkTransferScheduler.h"

#include "GameFramework/CharacterMovementComponent.h"

class ACharacter;

/** Server 对一个 Chunk 的协议订阅与增量积压；只在 Authority Game Thread 访问。 */
struct FServerChunkSubscription final
{
	FWorldChunkOffer Offer;
	bool bSnapshotAcknowledged = false;
	int32 SnapshotFailureCount = 0;
	double LastOfferSentSeconds = -DBL_MAX;
	int32 OfferSendCount = 0;
	UE::ElementSandbox::WorldStorage::Private::FWorldChunkOfferFlowControl OfferFlow;
};

/** 正在通过 Reliable Segment 窗口发送的完整 Chunk Snapshot。 */
struct FOutgoingChunk final
{
	FWorldChunkOffer Offer;
	TArray<uint8> Bytes;
	UE::ElementSandbox::NetBulk::FPayloadId BulkPayloadId;
	bool bQueuedToBulkScheduler = false;
};

/** Authority-only Chunk Offer、Snapshot 与 Live Delta 会话状态。 */
class FWorldChunkServerSession final
{
public:
	TMap<FWorldChunkCoord, FServerChunkSubscription> ServerSubscriptions;
	UE::ElementSandbox::WorldStorage::Private::FWorldChunkServerLiveDeltaQueue LiveDeltaQueue;
	TSet<FWorldChunkCoord> ActivationCoreChunks;
	TMap<FWorldChunkCoord, FWorldChunkOffer> PendingOffersToSend;
	TMap<FWorldChunkCoord, FOutgoingChunk> OutgoingChunks;
	TArray<FWorldChunkCoord> PendingSnapshotRequests;
	TSet<FWorldChunkCoord> SnapshotPreparations;
	UE::ElementSandbox::WorldStorage::Private::FWorldChunkLiveDeltaFlowControl LiveDeltaFlow;
	TMap<uint64, TSet<FWorldChunkCoord>> LiveDeltaBatchCoordsInFlight;
	uint64 NextBulkTransferId = 1;
	bool bClientEndpointReady = false;
};

/** Owner Client-only 磁盘缓存查询、Segment 组装与 Delta 顺序状态。 */
class FWorldChunkClientSession final
{
public:
	TMap<FWorldChunkCoord, UE::ElementSandbox::WorldStorage::Private::FWorldChunkSegmentAssembly> IncomingChunks;
	TMap<FWorldChunkCoord, FWorldChunkOffer> ClientOffersToCheck;
	TSet<FWorldChunkCoord> ClientCacheLookups;
	TArray<FWorldChunkOfferResponse> CompletedClientResponses;
	FString ServerFingerprint = TEXT("Loopback");
	UE::ElementSandbox::WorldStorage::Private::FWorldChunkClientLiveDeltaQueue LiveDeltaQueue;
};

/** 两端共享的本地 Pawn 观察样本；不承载协议窗口或持久化状态。 */
struct FWorldChunkObservationState final
{
	FWorldChunkCoord CurrentCenter;
	FVector CurrentLocation = FVector::ZeroVector;
	FVector CurrentForward = FVector::ForwardVector;
};

/** 首次 Activation Core 对本地输入和 Authority Movement 的成对门禁所有权。 */
class FWorldChunkActivationGateState final
{
public:
	TWeakObjectPtr<ACharacter> AuthorityGatedCharacter;
	uint8 AuthorityPreviousMovementMode = MOVE_Walking;
	uint8 AuthorityPreviousCustomMovementMode = 0;
	double LastActivationCoreProgressLogSeconds = -DBL_MAX;
	bool bActivationCoreReadyLogged = false;
	/** 首次连接放行后保持为 true；后续兴趣中心迁移只在后台流送，不能重新锁定 Gameplay。 */
	bool bInitialActivationGateSatisfied = false;
	bool bAuthorityMovementBlockedForActivationCore = false;
};

/** Component 的薄协调状态；角色不同的协议数据由三个组合对象分别拥有。 */
class FWorldChunkStreamingRuntime final
{
public:
	FWorldChunkServerSession Server;
	FWorldChunkClientSession Client;
	FWorldChunkObservationState Observation;
	FWorldChunkActivationGateState Activation;
	FWorldChunkStreamingStats Stats;
	double BeginPlaySeconds = 0.0;
};
