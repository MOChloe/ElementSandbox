#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectEntityHandle.h"

struct FWorldDestructionDefinition;

namespace UE::ElementSandbox::Destruction
{
	enum class EWorldDestructionTargetDomain : uint8
	{
		None,
		Building,
		WorldObject
	};

	enum class EWorldDestructionDamageMode : uint8
	{
		Additive,
		ExhaustDurability
	};

	/** 一次权威裁决得到的宿主身份。Handle、WorldEntityId 与 Revision 必须同时复验。 */
	struct ELEMENTSANDBOXDESTRUCTION_API FWorldDestructionTarget final
	{
		EWorldDestructionTargetDomain Domain = EWorldDestructionTargetDomain::None;
		FBuildEntityHandle Building;
		FWorldObjectEntityHandle WorldObject;
		FWorldEntityId WorldEntityId;
		uint32 SourceRevision = 0;
		double Distance = 0.0;

		bool IsSet() const
		{
			return WorldEntityId.IsSet() && SourceRevision != 0
				&& ((Domain == EWorldDestructionTargetDomain::Building && Building.IsSet())
					|| (Domain == EWorldDestructionTargetDomain::WorldObject && WorldObject.IsSet()));
		}
	};

	/**
	 * 只属于一次破坏命令的发射参数。它既不是 Fragment，也不是 Definition 或持久状态；
	 * 若本次伤害没有完成破坏，调用结束时直接丢弃。
	 */
	struct ELEMENTSANDBOXDESTRUCTION_API FWorldProductLaunchContext final
	{
		uint64 EventId = 0;
		FVector ImpactCenter = FVector::ZeroVector;
		float RadialStrength = 0.0f;
		float UpwardStrength = 0.0f;
		float DistanceFalloff = 1.0f;
		uint64 RandomSeed = 0;

		bool IsValid() const
		{
			return EventId != 0 && RandomSeed != 0 && !ImpactCenter.ContainsNaN()
				&& FMath::IsFinite(RadialStrength) && RadialStrength >= 0.0f
				&& FMath::IsFinite(UpwardStrength) && UpwardStrength >= 0.0f
				&& FMath::IsFinite(DistanceFalloff) && DistanceFalloff >= 0.0f;
		}
	};

	/** Product Sink 在 Prepare 阶段收到的不可变破坏快照。 */
	struct ELEMENTSANDBOXDESTRUCTION_API FWorldDestructionProductBatch final
	{
		FWorldDestructionTarget Target;
		FWorldEntityId SourceId;
		uint64 DestructionRevision = 0;
		FBox SourceBounds = FBox(ForceInit);
		const FWorldDestructionDefinition* Definition = nullptr;
		const FWorldProductLaunchContext* LaunchContext = nullptr;

		bool IsValid() const;
	};

	/**
	 * 跨域破坏的两阶段产品出口。Prepare 可失败并必须保持可回滚；源销毁成功后 Commit
	 * 只能发布已经准备好的状态，不允许再分配、查询或返回失败。
	 */
	class ELEMENTSANDBOXDESTRUCTION_API IWorldDestructionProductSink
	{
	public:
		virtual ~IWorldDestructionProductSink() = default;
		virtual bool Prepare(const FWorldDestructionProductBatch& Batch) = 0;
		virtual void Commit() = 0;
		virtual void Rollback() = 0;
	};

	struct ELEMENTSANDBOXDESTRUCTION_API FWorldDestructionRequest final
	{
		FWorldDestructionTarget Target;
		EWorldDestructionDamageMode DamageMode = EWorldDestructionDamageMode::Additive;
		float Damage = 0.0f;
		TOptional<FWorldProductLaunchContext> LaunchContext;
		IWorldDestructionProductSink* ProductSink = nullptr;

		bool IsValid() const
		{
			return Target.IsSet()
				&& (DamageMode == EWorldDestructionDamageMode::ExhaustDurability
					|| (FMath::IsFinite(Damage) && Damage > 0.0f))
				&& (!LaunchContext.IsSet() || LaunchContext->IsValid());
		}
	};
}
