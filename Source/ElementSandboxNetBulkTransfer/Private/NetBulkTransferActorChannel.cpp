#include "NetBulkTransferActorChannel.h"

#include "Engine/ActorChannel.h"
#include "Engine/NetConnection.h"
#include "GameFramework/Actor.h"
#include "NetBulkTransferScheduler.h"

namespace UE::ElementSandbox::NetBulk
{
namespace
{
int32 GetPayloadBudget(AActor* Owner, const int32 MaximumBytes, const int32 ReliableLimit)
{
	check(IsInGameThread());
	UNetConnection* Connection = Owner ? Owner->GetNetConnection() : nullptr;
	if (!Connection || Connection->GetConnectionState() != USOCK_Open || MaximumBytes <= 0)
	{
		return 0;
	}
	UActorChannel* Channel = Connection->FindActorChannelRef(Owner);
	if (!Channel || Channel->Closing || !Channel->IsNetReady())
	{
		return 0;
	}
	const int32 BunchBits = Connection->GetMaxSingleBunchSizeBits();
	const int32 PacketBits = Connection->MaxPacket * 8;
	if (BunchBits <= 0 || PacketBits <= 0)
	{
		return 0;
	}
	// IsNetReady 只表示当前还没超额，不能证明下一份大 RPC 发完后仍有余量。
	// QueuedBits 是 UE 的实际带宽债务（负数表示额度）；连同尚未 Flush 的 SendBuffer 计入。
	// RPC 头预留两包，另留四包给同帧稍后发送的 Character 调整和普通 Actor 属性。
	constexpr int32 RpcOverheadPackets = 2;
	constexpr int32 GameplayReservePackets = 4;
	const int64 AvailableWireBits = -static_cast<int64>(Connection->QueuedBits) - Connection->SendBuffer.GetNumBits();
	const int32 WireBunches = static_cast<int32>(FMath::Max<int64>(0,
		AvailableWireBits / PacketBits - RpcOverheadPackets - GameplayReservePackets));
	int32 PayloadBunches = FMath::Min3(
		FMath::DivideAndRoundUp(MaximumBytes * 8, BunchBits), WireBunches,
		FMath::Max(0, ReliableLimit - Channel->NumOutRec - RpcOverheadPackets - 1));
	while (PayloadBunches > 0 && Connection->IsPacketSequenceWindowFull(
		PayloadBunches + RpcOverheadPackets + GameplayReservePackets))
	{
		--PayloadBunches;
	}
	return FMath::Min(MaximumBytes, PayloadBunches * BunchBits / 8);
}
}

bool CanSendReliablePayload(AActor* Owner, const int32 PayloadBytes)
{
	// 控制/Delta 可以使用 Bulk 预留的另一半，避免 Bulk 持续装填时反过来饿死控制消息。
	return PayloadBytes > 0 && GetReliablePayloadBudget(Owner, PayloadBytes) >= PayloadBytes;
}

int32 GetReliablePayloadBudget(AActor* Owner, const int32 MaximumBytes)
{
	return GetPayloadBudget(Owner, MaximumBytes, RELIABLE_BUFFER - 1);
}

void BindActorChannelTransport(FConnectionScheduler& Scheduler, AActor* Owner)
{
	const TWeakObjectPtr<AActor> WeakOwner(Owner);
	Scheduler.SetCanSendSegment([WeakOwner](const int32 Bytes)
	{
		// Bulk 只使用半个 Reliable Buffer，让移动、控制和 Live Delta 仍可入队。
		return GetPayloadBudget(WeakOwner.Get(), Bytes, RELIABLE_BUFFER / 2) >= Bytes;
	});
}
}
