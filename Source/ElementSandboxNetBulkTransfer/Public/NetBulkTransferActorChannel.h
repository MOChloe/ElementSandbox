#pragma once

#include "CoreMinimal.h"

class AActor;

namespace UE::ElementSandbox::NetBulk
{
	class FConnectionScheduler;

	/** GameThread-only：读取当前 Owner Channel 的带宽/可靠缓冲区余量，不创建 Channel。 */
		ELEMENTSANDBOXNETBULKTRANSFER_API bool CanSendReliablePayload(AActor* Owner, int32 PayloadBytes);
		/** 返回当前可提交的 Payload 上限；为角色移动确认与 Actor 复制保留连接本帧的发送余量。 */
		ELEMENTSANDBOXNETBULKTRANSFER_API int32 GetReliablePayloadBudget(AActor* Owner, int32 MaximumBytes);

	/** 弱引用 Owner；Travel 或重连后每次查询当前连接，不保存失效的 Channel 指针。 */
	ELEMENTSANDBOXNETBULKTRANSFER_API void BindActorChannelTransport(FConnectionScheduler& Scheduler, AActor* Owner);
}
