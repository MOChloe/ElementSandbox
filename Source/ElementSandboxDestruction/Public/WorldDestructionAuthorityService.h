#pragma once

#include "CoreMinimal.h"
#include "WorldDestructionTypes.h"

class UWorld;

namespace UE::ElementSandbox::Destruction
{
	/**
	 * Building 与 WorldObject 共用的服务器破坏裁决。服务不 Tick、不保存 Handle，也不读取 Actor；
	 * 调用方必须提供服务器权威视点或已经裁决的 Target。
	 */
	class ELEMENTSANDBOXDESTRUCTION_API FWorldDestructionAuthorityService final
	{
	public:
		static bool TryResolveNearestTarget(
			UWorld& World,
			const FVector& ViewOrigin,
			const FVector& UnitDirection,
			const FVector& InstigatorLocation,
			double FocusDistance,
			FWorldDestructionTarget& OutTarget);

		/** 同步执行一次伤害和两阶段产品事务；客户端、stale 身份或非法请求会被拒绝。 */
		static bool TryApplyRequest(UWorld& World, const FWorldDestructionRequest& Request);

		/** Fire 等已完成自身状态裁决的系统可复用同一两阶段产品事务。 */
		static bool TryConvertResolvedSource(
			UWorld& World,
			const FWorldDestructionProductBatch& Batch,
			TFunctionRef<bool()> DestroySource,
			TFunctionRef<bool()> IsSourceAlive,
			IWorldDestructionProductSink* ProductSink = nullptr);
	};
}
