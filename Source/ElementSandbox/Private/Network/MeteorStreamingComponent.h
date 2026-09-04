#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "NetBulkTransferScheduler.h"

#include "MeteorStreamingComponent.generated.h"

namespace UE::ElementSandbox::Meteor
{
	struct FMeteorTrajectoryPage;
	struct FMeteorTrajectoryActivation;
	struct FMeteorSettlementMapping;
}

USTRUCT()
struct FMeteorNetPageId final
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 BurstId = 0;
	UPROPERTY()
	uint64 PageId = 0;
	UPROPERTY()
	uint32 Revision = 0;

	bool IsValid() const { return BurstId != 0 && PageId != 0 && Revision != 0; }
	friend bool operator==(const FMeteorNetPageId&, const FMeteorNetPageId&) = default;
	friend uint32 GetTypeHash(const FMeteorNetPageId& Id)
	{
		return HashCombineFast(GetTypeHash(Id.BurstId), HashCombineFast(GetTypeHash(Id.PageId), Id.Revision));
	}
};

USTRUCT()
struct FMeteorNetPageOffer final
{
	GENERATED_BODY()

	UPROPERTY()
	FMeteorNetPageId Id;
	UPROPERTY()
	int32 TotalBytes = 0;
	UPROPERTY()
	uint16 SegmentCount = 0;
	UPROPERTY()
	uint32 PayloadHash = 0;
};

USTRUCT()
struct FMeteorNetPageSegment final
{
	GENERATED_BODY()

	UPROPERTY()
	FMeteorNetPageId Id;
	UPROPERTY()
	uint16 SegmentIndex = 0;
	UPROPERTY()
	uint16 SegmentCount = 0;
	UPROPERTY()
	uint32 PayloadHash = 0;
	UPROPERTY()
	TArray<uint8> Bytes;
};

USTRUCT()
struct FMeteorNetPageActivation final
{
	GENERATED_BODY()

	UPROPERTY()
	FMeteorNetPageId Id;
	UPROPERTY()
	double AuthorityStartTimeSeconds = 0.0;
	UPROPERTY()
	FWorldEntityId SourceWorldEntityId;
	UPROPERTY()
	uint32 SourceTombstoneRevision = 0;
	UPROPERTY()
	TArray<uint32> Ordinals;
};

USTRUCT()
struct FMeteorNetLaneControl final
{
	GENERATED_BODY()

	UPROPERTY()
	bool bCancellation = false;
	UPROPERTY()
	FMeteorNetPageActivation Activation;
};

USTRUCT()
struct FMeteorNetSettlement final
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 BurstId = 0;
	UPROPERTY()
	uint32 DebrisOrdinal = MAX_uint32;
	UPROPERTY()
	FWorldEntityId WorldEntityId;

	bool IsValid() const
	{
		return BurstId != 0 && DebrisOrdinal != MAX_uint32 && WorldEntityId.IsSet();
	}
};

USTRUCT()
struct FMeteorNetLaneControlBatch final
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 Sequence = 0;
	UPROPERTY()
	TArray<FMeteorNetLaneControl> Controls;
	UPROPERTY()
	TArray<FMeteorNetSettlement> Settlements;
};

/**
	 * Meteor Activate/Cancel/Settlement 共用应用层窗口，不能在落地回调中绕过背压直发 RPC。
	 * 每批同时推进源控制与落地确认，Client ACK 前最多保留两个批次（每批可含多个 Bunch）。
 */
struct FMeteorLaneControlSendWindow final
{
	static constexpr int32 MaximumRecordsPerBatch = 96;
	static constexpr int32 MaximumOrdinalsPerRecord = 256;
	static constexpr int32 MaximumOrdinalsPerBatch = 768;
	static constexpr int32 MaximumSettlementsPerBatch = 512;
	static constexpr int32 MaximumInFlightBatches = 2;

	void Enqueue(FMeteorNetPageActivation Activation, bool bCancellation);
	void EnqueueSettlements(TConstArrayView<FMeteorNetSettlement> Settlements);
	/** 只检查下一批的有界前缀，发送准入失败不消费任何记录。 */
	int32 GetNextBatchEstimatedBytes() const;
	bool TryBuildBatch(FMeteorNetLaneControlBatch& OutBatch);
	bool Acknowledge(uint32 Sequence);
	void Reset();
	int32 GetPendingRecordCount() const { return PendingControls.Num() - PendingControlHead; }
	int32 GetPendingSettlementCount() const { return PendingSettlements.Num() - PendingSettlementHead; }
	int32 GetInFlightBatchCount() const { return InFlightSequences.Num(); }

private:
	TArray<FMeteorNetLaneControl> PendingControls;
	TArray<FMeteorNetSettlement> PendingSettlements;
	TSet<uint32> InFlightSequences;
	int32 PendingControlHead = 0;
	int32 PendingSettlementHead = 0;
	uint32 NextSequence = 1;
};

/** Owner-only 轨迹页流式端点；领域 Payload 始终先经过中性分段/ACK 调度器。 */
UCLASS(ClassGroup=(Network), meta=(BlueprintSpawnableComponent))
class UMeteorStreamingComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UMeteorStreamingComponent();
	void SetBulkTransferScheduler(
		TSharedPtr<UE::ElementSandbox::NetBulk::FConnectionScheduler> InScheduler);
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void HandlePagePrepared(const UE::ElementSandbox::Meteor::FMeteorTrajectoryPage& Page);
	void HandlePageActivated(const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation& Activation);
	void HandlePageCanceled(const UE::ElementSandbox::Meteor::FMeteorTrajectoryActivation& Cancellation);
	void HandleSettlementPublished(TConstArrayView<UE::ElementSandbox::Meteor::FMeteorSettlementMapping> Mappings);
	void TryFinalizeClientPage(const FMeteorNetPageId& Id);
	void DrainClientSourceCausalGates();
	void PublishClientActivation(const FMeteorNetPageActivation& Activation);
	void PublishClientSettlements(TConstArrayView<FMeteorNetSettlement> Settlements);
	void QueueClientActivation(FMeteorNetPageActivation Activation);
	void ApplyClientCancellation(FMeteorNetPageActivation Cancellation);

	/** Owner Client 在本组件 BeginPlay 后调用，Authority 才开始回放和订阅；可靠，重复调用无副作用。 */
	UFUNCTION(Server, Reliable)
	void ServerStartMeteorStreaming();
	/** Authority 向已建立端点的 Owner Client 可靠发布页元数据；客户端拒绝尺寸和哈希冲突。 */
	UFUNCTION(Client, Reliable)
	void ClientOfferMeteorPage(FMeteorNetPageOffer Offer);
	/** Authority 向 Owner Client 可靠发送分段；只接收已知页且尺寸、索引与哈希一致的分段。 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMeteorSegment(FMeteorNetPageSegment Segment);
	/** Authority 向 Owner Client 可靠取消整页，重复和陈旧页由客户端生命周期规则处理。 */
	UFUNCTION(Client, Reliable)
	void ClientCancelMeteorPage(FMeteorNetPageId Id);
	/** Authority 经共享窗口向 Owner Client 可靠发布激活、取消与实体化确认；无效身份、时间和序号拒绝应用。 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMeteorLaneControlBatch(FMeteorNetLaneControlBatch Batch);
	/** Owner Client 向 Authority 可靠确认分段接收；只释放本连接当前在途分段，重复 ACK 无效。 */
	UFUNCTION(Server, Reliable)
	void ServerAckMeteorSegment(FMeteorNetPageId Id, uint16 SegmentIndex);
	/** Owner Client 向 Authority 可靠确认控制批接收；只接受本连接尚在途的序号。 */
	UFUNCTION(Server, Reliable)
	void ServerAckMeteorLaneControlBatch(uint32 Sequence);
	/** Camera Interest 不参与权威裁决，只影响未来页的带宽优先级，因此使用 Unreliable。 */
	UFUNCTION(Server, Unreliable)
	void ServerSubmitMeteorCameraInterest(
		FVector_NetQuantize100 Location,
		FVector_NetQuantizeNormal Direction,
		float FieldOfViewDegrees);

	struct FServerPayloadState final
	{
		FMeteorNetPageOffer Offer;
		int32 AckedSegments = 0;
		bool bOfferSent = false;
	};
	struct FClientAssembly final
	{
		FMeteorNetPageOffer Offer;
		TArray<TArray<uint8>> Segments;
		TBitArray<> Received;
		TSharedPtr<UE::ElementSandbox::Meteor::FMeteorTrajectoryPage> DecodedPage;
		TSet<uint32> ActivatedOrdinals;
		TSet<uint32> CanceledOrdinals;
	};
	struct FClientSourceCausalGate final
	{
		uint32 TombstoneRevision = 0;
		uint32 AppliedTombstoneRevision = 0;
		FBuildEntityHandle BuildingEntity;
		TArray<FMeteorNetPageActivation> PendingActivations;
		bool bCommitQueued = false;
		bool bProjectionQueued = false;
	};
	TSharedPtr<UE::ElementSandbox::NetBulk::FConnectionScheduler> ServerScheduler;
	bool bServerStreamingStarted = false;
	TMap<FMeteorNetPageId, FServerPayloadState> ServerPayloads;
	FMeteorLaneControlSendWindow ServerLaneControlWindow;
	TMap<FMeteorNetPageId, FClientAssembly> ClientAssemblies;
	TMap<FWorldEntityId, FClientSourceCausalGate> ClientSourceCausalGates;
	TArray<FWorldEntityId> ClientPendingSourceCommits;
	TArray<FWorldEntityId> ClientAwaitingSourceProjection;
	TSet<FMeteorNetPageId> ClientCanceledPages;
	int32 ClientPendingSourceCommitHead = 0;
	int32 ClientAwaitingSourceProjectionHead = 0;
	FDelegateHandle PagePreparedHandle;
	FDelegateHandle PageActivatedHandle;
	FDelegateHandle PageCanceledHandle;
	FDelegateHandle SettlementPublishedHandle;
	FVector ServerInterestLocation = FVector::ZeroVector;
	FVector ServerInterestDirection = FVector::ForwardVector;
	float ServerInterestFov = 90.0f;
	double NextInterestSendSeconds = 0.0;
	double NextTelemetrySeconds = 0.0;
	uint64 ServerOfferedPageCount = 0;
	uint64 ServerOfferedLaneCount = 0;
	uint64 ServerOfferedByteCount = 0;
	uint64 ServerActivatedLaneCount = 0;
	uint64 ClientDecodedPageCount = 0;
	uint64 ClientDecodedLaneCount = 0;
	uint64 ClientAppliedLaneCount = 0;
	int32 LastClientSourceCommitCount = 0;
	int32 LastClientSourceReleasedLaneCount = 0;
	double LastClientSourceGateMilliseconds = 0.0;
};
