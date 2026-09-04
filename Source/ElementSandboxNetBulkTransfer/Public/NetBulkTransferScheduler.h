#pragma once

#include "CoreMinimal.h"

namespace UE::ElementSandbox::NetBulk
{
	constexpr int32 DefaultSegmentBytes = 24 * 1024;

	enum class ETransferClass : uint8
	{
		GameplayControl,
		MeteorUrgent,
		WorldStorage,
		MeteorBackground,
		Count
	};

	struct ELEMENTSANDBOXNETBULKTRANSFER_API FPayloadId final
	{
		uint32 Domain = 0;
		uint64 Transfer = 0;
		uint32 Revision = 0;

		bool IsValid() const { return Domain != 0 && Transfer != 0 && Revision != 0; }
		friend bool operator==(const FPayloadId&, const FPayloadId&) = default;
		friend uint32 GetTypeHash(const FPayloadId& Id)
		{
			return HashCombineFast(HashCombineFast(Id.Domain, GetTypeHash(Id.Transfer)), Id.Revision);
		}
	};

	struct ELEMENTSANDBOXNETBULKTRANSFER_API FSegment final
	{
		FPayloadId PayloadId;
		ETransferClass TransferClass = ETransferClass::MeteorBackground;
		uint16 SegmentIndex = 0;
		uint16 SegmentCount = 0;
		uint32 PayloadHash = 0;
		TArray<uint8> Bytes;

		bool IsValid() const
		{
			return PayloadId.IsValid() && SegmentCount > 0 && SegmentIndex < SegmentCount
				&& !Bytes.IsEmpty() && Bytes.Num() <= DefaultSegmentBytes;
		}
	};

	using FSegmentDispatch = TFunction<void(FSegment&&)>;
	using FCanSendSegment = TFunction<bool(int32)>;

	/**
	 * 单连接、无领域语义的分段与加权公平队列。所有热路径节点按 payload 入队时一次分配；
	 * 发送额度由连接适配器提供，接收 ACK 只回收在途身份，不阻挡后续小段。
	 * GameplayControl 永远先于 Bulk，各类通过加权轮转避免饥饿。仅在 GameThread 使用。
	 */
	class ELEMENTSANDBOXNETBULKTRANSFER_API FConnectionScheduler final
	{
	public:
		FConnectionScheduler() = default;
		FConnectionScheduler(const FConnectionScheduler&) = delete;
		FConnectionScheduler& operator=(const FConnectionScheduler&) = delete;
		bool Enqueue(
			ETransferClass TransferClass,
			FPayloadId PayloadId,
			TConstArrayView<uint8> Payload,
			int32 SegmentBytes = DefaultSegmentBytes,
			FSegmentDispatch Dispatch = {});
		/** 在实际出队前查询传输余量；拒绝时保留原分段与游标，下一次 Pump 可继续。 */
		void SetCanSendSegment(FCanSendSegment InCanSendSegment);
		bool TryDequeue(FSegment& OutSegment);
		/** 由任一共享该连接 Scheduler 的端点 Pump；实际 RPC 通过 Payload 自己的回调回到所属领域。 */
		bool TryDispatch();
		bool Acknowledge(FPayloadId PayloadId, uint16 SegmentIndex);
		void Cancel(FPayloadId PayloadId);
		void Reset();

		int32 GetInFlightCount() const { return InFlight.Num(); }
		int32 GetQueuedSegmentCount() const;
		int64 GetQueuedBytes() const;

	private:
		struct FQueuedPayload final
		{
			FPayloadId Id;
			ETransferClass TransferClass = ETransferClass::MeteorBackground;
			uint32 PayloadHash = 0;
			int32 SegmentBytes = DefaultSegmentBytes;
			int32 NextSegment = 0;
			TArray<uint8> Bytes;
			TSharedPtr<FSegmentDispatch> Dispatch;

			int32 SegmentCount() const
			{
				return FMath::DivideAndRoundUp(Bytes.Num(), SegmentBytes);
			}
		};

		struct FInFlightKey final
		{
			FPayloadId PayloadId;
			uint16 SegmentIndex = 0;
			friend bool operator==(const FInFlightKey&, const FInFlightKey&) = default;
			friend uint32 GetTypeHash(const FInFlightKey& Key)
			{
				return HashCombineFast(GetTypeHash(Key.PayloadId), Key.SegmentIndex);
			}
		};

		bool TryDequeueInternal(
			FSegment& OutSegment,
			TSharedPtr<FSegmentDispatch>* OutDispatch);
		bool TryDequeueFromClass(
			ETransferClass TransferClass,
			FSegment& OutSegment,
			TSharedPtr<FSegmentDispatch>* OutDispatch);

		TArray<TUniquePtr<FQueuedPayload>> Queues[static_cast<int32>(ETransferClass::Count)];
		TSet<FInFlightKey> InFlight;
		int32 WeightedCursor = 0;
		FCanSendSegment CanSendSegment;
	};
}
