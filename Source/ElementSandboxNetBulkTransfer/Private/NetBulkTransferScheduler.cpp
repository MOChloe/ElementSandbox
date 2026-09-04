#include "NetBulkTransferScheduler.h"

#include "Misc/Crc.h"

namespace UE::ElementSandbox::NetBulk
{
bool FConnectionScheduler::Enqueue(
	const ETransferClass TransferClass,
	const FPayloadId PayloadId,
	const TConstArrayView<uint8> Payload,
	const int32 SegmentBytes,
	FSegmentDispatch Dispatch)
{
	const int32 ClassIndex = static_cast<int32>(TransferClass);
	if (!PayloadId.IsValid() || Payload.IsEmpty() || ClassIndex < 0
		|| ClassIndex >= static_cast<int32>(ETransferClass::Count)
		|| SegmentBytes <= 0 || SegmentBytes > DefaultSegmentBytes
		|| Payload.Num() > SegmentBytes * MAX_uint16)
	{
		return false;
	}
	for (int32 Index = 0; Index < static_cast<int32>(ETransferClass::Count); ++Index)
	{
		for (const TUniquePtr<FQueuedPayload>& Existing : Queues[Index])
		{
			if (Existing && Existing->Id == PayloadId)
			{
				return false;
			}
		}
	}
	for (const FInFlightKey& Existing : InFlight)
	{
		if (Existing.PayloadId == PayloadId)
		{
			return false;
		}
	}

	TUniquePtr<FQueuedPayload> Queued = MakeUnique<FQueuedPayload>();
	Queued->Id = PayloadId;
	Queued->TransferClass = TransferClass;
	Queued->SegmentBytes = SegmentBytes;
	Queued->Bytes.Append(Payload.GetData(), Payload.Num());
	Queued->PayloadHash = FCrc::MemCrc32(Queued->Bytes.GetData(), Queued->Bytes.Num());
	if (Dispatch)
	{
		Queued->Dispatch = MakeShared<FSegmentDispatch>(MoveTemp(Dispatch));
	}
	Queues[ClassIndex].Add(MoveTemp(Queued));
	return true;
}

void FConnectionScheduler::SetCanSendSegment(FCanSendSegment InCanSendSegment)
{
	CanSendSegment = MoveTemp(InCanSendSegment);
}

bool FConnectionScheduler::TryDequeue(FSegment& OutSegment)
{
	return TryDequeueInternal(OutSegment, nullptr);
}

bool FConnectionScheduler::TryDispatch()
{
	// 生产 RPC 必须绑定传输适配器。纯值测试可直接使用 TryDequeue。
	if (!CanSendSegment)
	{
		return false;
	}
	FSegment Segment;
	TSharedPtr<FSegmentDispatch> Dispatch;
	if (!TryDequeueInternal(Segment, &Dispatch))
	{
		return false;
	}
	if (Dispatch && *Dispatch)
	{
		(*Dispatch)(MoveTemp(Segment));
	}
	else
	{
		// 共享 Pump 不允许静默吞掉没有接收端的 Payload。
		Acknowledge(Segment.PayloadId, Segment.SegmentIndex);
	}
	return true;
}

bool FConnectionScheduler::TryDequeueInternal(
	FSegment& OutSegment,
	TSharedPtr<FSegmentDispatch>* OutDispatch)
{
	OutSegment = {};
	if (OutDispatch)
	{
		OutDispatch->Reset();
	}
	if (TryDequeueFromClass(ETransferClass::GameplayControl, OutSegment, OutDispatch))
	{
		return true;
	}

	// 固定权重环比不断累积、再每类只发一段的伪 DRR 更直观，也真正实现 8:4:1。
	static constexpr ETransferClass WeightedClasses[] = {
		ETransferClass::MeteorUrgent, ETransferClass::MeteorUrgent,
		ETransferClass::MeteorUrgent, ETransferClass::MeteorUrgent,
		ETransferClass::MeteorUrgent, ETransferClass::MeteorUrgent,
		ETransferClass::MeteorUrgent, ETransferClass::MeteorUrgent,
		ETransferClass::WorldStorage, ETransferClass::WorldStorage,
		ETransferClass::WorldStorage, ETransferClass::WorldStorage,
		ETransferClass::MeteorBackground};
	for (int32 Attempt = 0; Attempt < UE_ARRAY_COUNT(WeightedClasses); ++Attempt)
	{
		const ETransferClass TransferClass = WeightedClasses[WeightedCursor];
		WeightedCursor = (WeightedCursor + 1) % UE_ARRAY_COUNT(WeightedClasses);
		if (TryDequeueFromClass(TransferClass, OutSegment, OutDispatch))
		{
			return true;
		}
	}
	return false;
}

bool FConnectionScheduler::Acknowledge(const FPayloadId PayloadId, const uint16 SegmentIndex)
{
	return InFlight.Remove({PayloadId, SegmentIndex}) > 0;
}

void FConnectionScheduler::Cancel(const FPayloadId PayloadId)
{
	for (int32 ClassIndex = 0; ClassIndex < static_cast<int32>(ETransferClass::Count); ++ClassIndex)
	{
		Queues[ClassIndex].RemoveAllSwap(
			[PayloadId](const TUniquePtr<FQueuedPayload>& Payload)
			{
				return Payload && Payload->Id == PayloadId;
			}, EAllowShrinking::No);
	}
	for (auto Iterator = InFlight.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator->PayloadId == PayloadId)
		{
			Iterator.RemoveCurrent();
		}
	}
}

void FConnectionScheduler::Reset()
{
	for (int32 ClassIndex = 0; ClassIndex < static_cast<int32>(ETransferClass::Count); ++ClassIndex)
	{
		Queues[ClassIndex].Reset();
	}
	InFlight.Reset();
	WeightedCursor = 0;
}

int32 FConnectionScheduler::GetQueuedSegmentCount() const
{
	int32 Count = 0;
	for (int32 ClassIndex = 0; ClassIndex < static_cast<int32>(ETransferClass::Count); ++ClassIndex)
	{
		for (const TUniquePtr<FQueuedPayload>& Payload : Queues[ClassIndex])
		{
			Count += Payload ? Payload->SegmentCount() - Payload->NextSegment : 0;
		}
	}
	return Count;
}

int64 FConnectionScheduler::GetQueuedBytes() const
{
	int64 Bytes = 0;
	for (int32 ClassIndex = 0; ClassIndex < static_cast<int32>(ETransferClass::Count); ++ClassIndex)
	{
		for (const TUniquePtr<FQueuedPayload>& Payload : Queues[ClassIndex])
		{
			if (Payload)
			{
				Bytes += Payload->Bytes.Num()
					- FMath::Min(Payload->Bytes.Num(), Payload->NextSegment * Payload->SegmentBytes);
			}
		}
	}
	return Bytes;
}

bool FConnectionScheduler::TryDequeueFromClass(
	const ETransferClass TransferClass,
	FSegment& OutSegment,
	TSharedPtr<FSegmentDispatch>* OutDispatch)
{
	TArray<TUniquePtr<FQueuedPayload>>& Queue = Queues[static_cast<int32>(TransferClass)];
	while (!Queue.IsEmpty())
	{
		FQueuedPayload* Payload = Queue[0].Get();
		if (!Payload || Payload->NextSegment >= Payload->SegmentCount())
		{
			Queue.RemoveAt(0, EAllowShrinking::No);
			continue;
		}
		const int32 SegmentIndex = Payload->NextSegment;
		const int32 Offset = SegmentIndex * Payload->SegmentBytes;
		const int32 Count = FMath::Min(Payload->SegmentBytes, Payload->Bytes.Num() - Offset);
		if (CanSendSegment && !CanSendSegment(Count))
		{
			return false;
		}
		++Payload->NextSegment;
		OutSegment.PayloadId = Payload->Id;
		OutSegment.TransferClass = TransferClass;
		OutSegment.SegmentIndex = static_cast<uint16>(SegmentIndex);
		OutSegment.SegmentCount = static_cast<uint16>(Payload->SegmentCount());
		OutSegment.PayloadHash = Payload->PayloadHash;
		OutSegment.Bytes.Append(Payload->Bytes.GetData() + Offset, Count);
		if (OutDispatch)
		{
			*OutDispatch = Payload->Dispatch;
		}
		InFlight.Add({Payload->Id, OutSegment.SegmentIndex});
		if (Payload->NextSegment >= Payload->SegmentCount())
		{
			Queue.RemoveAt(0, EAllowShrinking::No);
		}
		return true;
	}
	return false;
}
}
