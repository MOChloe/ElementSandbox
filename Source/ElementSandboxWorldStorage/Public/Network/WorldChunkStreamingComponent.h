#pragma once

#include "Chunk/WorldChunkTypes.h"
#include "Components/ActorComponent.h"
#include "Templates/PimplPtr.h"
#include "WorldStorageSubsystem.h"

#include "WorldChunkStreamingComponent.generated.h"

class FWorldChunkStreamingRuntime;
namespace UE::ElementSandbox::NetBulk
{
	class FConnectionScheduler;
}

USTRUCT(BlueprintType)
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldChunkStreamingStats final
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 OfferedChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AcknowledgedChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 PendingChunkCount = 0;

	/** Authority 已提交但尚待向本连接发送的对象变化；与 Snapshot 加载进度分开。 */
	UPROPERTY(BlueprintReadOnly)
	int32 PendingLiveDeltaCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 SegmentsInFlight = 0;

	UPROPERTY(BlueprintReadOnly)
	int64 PayloadBytesReceived = 0;

	UPROPERTY(BlueprintReadOnly)
	int64 PayloadBytesSent = 0;

	/** 首次 Activation Core 完成后保持为 true；不是当前兴趣 Core 的实时完成状态。 */
	UPROPERTY(BlueprintReadOnly)
	bool bActivationCoreReady = false;

	UPROPERTY(BlueprintReadOnly)
	double ActivationCoreReadySeconds = 0.0;

	UPROPERTY(BlueprintReadOnly)
	int32 ActivationCoreChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 ActivationCoreAcknowledgedChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 ActivationCoreAuthorityReadyChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	FWorldChunkCoord InterestCenter;

	UPROPERTY(BlueprintReadOnly)
	int64 CompleteStructureCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int64 BuildingEntityCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int64 WorldObjectEntityCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityResidentEntityCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityResidentChunkCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityPendingLoadCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityPendingInjectionCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityDirtyEntityCount = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 AuthorityAwakePhysicsPinnedEntityCount = 0;

	UPROPERTY(BlueprintReadOnly)
	double AuthorityOldestAwakePhysicsPinSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly)
	double AuthorityLastInjectionMilliseconds = 0.0;

	UPROPERTY(BlueprintReadOnly)
	double AuthorityLastStepMilliseconds = 0.0;

	UPROPERTY(BlueprintReadOnly)
	int64 WorldSimulationTimeMilliseconds = 0;

	UPROPERTY(BlueprintReadOnly)
	bool bCheckpointInFlight = false;
};

/**
 * PlayerController 的 Owner-only Chunk 协议端点。
 * Authority 只根据权威 Pawn 位置建立 Residency；Client 坐标从不进入服务器裁决。
 */
UCLASS(ClassGroup = (Network), meta = (BlueprintSpawnableComponent))
class ELEMENTSANDBOXWORLDSTORAGE_API UWorldChunkStreamingComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWorldChunkStreamingComponent();
	virtual ~UWorldChunkStreamingComponent() override;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
							   FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure)
	FWorldChunkStreamingStats GetStreamingStats() const;
	/**
	 * Authority-only：目标 Chunk 已有 Client ACK 基线且 Server 运行态可修改时返回 true。
	 * Gameplay 在消耗物品或提交世界 Mutation 前调用，避免在后台流送窗口里丢失 Live Delta。
	 */
	bool IsAuthorityChunkReadyForLiveMutation(FWorldChunkCoord Coord) const;
	/** PlayerController 组合根注入与 Meteor 共用的每连接 Bulk 调度器。 */
	void SetBulkTransferScheduler(
		TSharedPtr<UE::ElementSandbox::NetBulk::FConnectionScheduler> InScheduler);

	/** PlayerController 的 Possess/UnPossess 钩子；换 Pawn 时更新兴趣中心，但已完成的首次可玩门禁不会重置。 */
	void NotifyPawnChanged();

private:
	/** Authority 调用、Owner Client 执行；可靠发布有限批 Offer，客户端拒绝超量或无效元数据。 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveChunkOffers(const TArray<FWorldChunkOffer>& Offers);

	/** Owner Client 在端点初始化后调用、Authority 执行；可靠解锁 Offer 发布，失效或非权威 Storage 时忽略。 */
	UFUNCTION(Server, Reliable)
	void ServerMarkChunkStreamingEndpointReady();

	/** Owner Client 调用、Authority 执行；可靠返回缓存命中/快照请求，只接受当前订阅的同 Revision 响应。 */
	UFUNCTION(Server, Reliable)
	void ServerRespondToChunkOffers(const TArray<FWorldChunkOfferResponse>& Responses);

	/** Authority 调用、Owner Client 执行；可靠发送一个快照分段，任何尺寸、索引或哈希异常都会拒绝整份快照。 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveChunkSegment(const FWorldChunkPayloadSegment& Segment);

		/** Owner Client 调用、Authority 执行；可靠回收分段接收记录，陈旧 ACK 静默丢弃，不限流后续段。 */
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeChunkSegment(FWorldChunkCoord Coord, uint32 Revision, int32 SegmentIndex);

	/** Owner Client 成功注入完整快照后调用、Authority 执行；可靠提交订阅 ACK，仅接受当前 Revision。 */
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeChunk(FWorldChunkCoord Coord, uint32 Revision);

	/** Owner Client 在组装、校验或注入失败后调用、Authority 执行；可靠触发有界重试，陈旧 Revision 不处理。 */
	UFUNCTION(Server, Reliable)
	void ServerRejectChunkSnapshot(FWorldChunkCoord Coord, uint32 Revision);

	/** Authority 调用、Owner Client 执行；可靠发送严格递增的 Delta Batch，乱序或非法批次拒绝应用。 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveLiveDeltaBatch(uint64 Sequence, const TArray<FWorldChunkLiveDelta>& Deltas);

	/** Owner Client 调用、Authority 执行；可靠关闭指定 Delta 窗口，失败时回退到完整 Snapshot。 */
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeLiveDeltaBatch(uint64 Sequence, bool bApplied);

	/** Authority 调用、Owner Client 执行的低频不可靠 HUD 投影；丢包不影响协议正确性。 */
	UFUNCTION(Client, Unreliable)
	void ClientReceiveStreamingStats(FWorldChunkStreamingStats AuthorityStats);

	void UpdateAuthoritativeResidencySource(bool bForce);
	void RefreshServerOffers();
	void PumpServerOfferQueue();
	void PumpClientLiveDeltaBatch();
	bool ApplyClientLiveDeltaBatch(TConstArrayView<FWorldChunkLiveDelta> Deltas);
	void PumpClientCacheLookups();
	void PumpServerSnapshotPreparations();
		void PumpServerPayload();
	void PumpServerLiveDeltas();
	void HandleAuthorityMutation(const FWorldStorageEntityMutation& Mutation);
	void RefreshServerStreamingStats();
	void RefreshActivationCoreReadiness();
	void ApplyClientActivationGate(bool bReady);
	void ApplyAuthorityActivationGate(bool bReady);
	FString GetClientCacheRoot(const FGuid& WorldId) const;

	TPimplPtr<FWorldChunkStreamingRuntime> Runtime;
	TSharedPtr<UE::ElementSandbox::NetBulk::FConnectionScheduler> BulkTransferScheduler;
	TWeakObjectPtr<UWorldStorageSubsystem> WorldStorage;
	FWorldResidencySourceHandle ResidencySource;
	FDelegateHandle MutationHandle;
	double OfferAccumulator = 0.0;
	/**
	 * SetIgnoreInput 是计数门禁。该所有权不能放在每次 BeginPlay 重建的
	 * 协议 Runtime 中，否则 ClientTravel 复用 PlayerController 时会重复加锁。
	 */
	bool bClientInputBlockedForActivationCore = false;

	static constexpr int32 SegmentPayloadBytes = 24 * 1024;
	static constexpr int32 MaximumOffersPerRpc = 32;
	/** Offer 从发布到 Have 或完整 Snapshot ACK 都占窗口，避免外围兴趣区淹没 Reliable Buffer。 */
	static constexpr int32 MaximumPublishedOffersInFlight = 64;
	static constexpr int32 MaximumLiveDeltaPayloadBytes = 64 * 1024;
	/** 两批应用窗口下的单批上限；Tombstone/ProjectionRemove 会按领域批量提交。 */
	static constexpr int32 MaximumLiveDeltasPerBatch = 256;
	static constexpr int32 MaximumLiveDeltaBatchBytes = 96 * 1024;
	static constexpr double OfferIntervalSeconds = 1.0;
	/** Reliable RPC 只保证已进入有效 Actor Channel 后的传输；应用层 Offer 必须在未 ACK 时自恢复。 */
	static constexpr double OfferResponseTimeoutSeconds = 1.0;

	friend class FWorldChunkStreamingRuntime;
};
