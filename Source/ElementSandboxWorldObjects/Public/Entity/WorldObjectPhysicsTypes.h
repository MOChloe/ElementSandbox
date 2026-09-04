#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectFragment.h"

#include "WorldObjectPhysicsTypes.generated.h"

namespace UE::ElementSandbox::WorldObjects::Physics
{
	inline constexpr float DefaultRestitution = 0.15f;
	inline constexpr float DefaultLinearDamping = 0.15f;
	inline constexpr float DefaultAngularDamping = 0.30f;
	inline constexpr float DefaultFriction = 0.65f;
	inline constexpr float DefaultMassKg = 1.0f;
	inline constexpr double PawnPushVelocityFraction = 1.20;
	inline constexpr double MaxPawnPushVelocity = 600.0;
	inline constexpr double CharacterContactMassKg = 80.0;

	/** LooseDebris 专用的受控水平让路速度；不会随接触帧数持续叠加。 */
	inline FVector ComputePawnPushVelocity(const FVector& PawnVelocity, const float BodyMassKg)
	{
		if (PawnVelocity.ContainsNaN() || !FMath::IsFinite(BodyMassKg) || BodyMassKg <= UE_SMALL_NUMBER)
		{
			return FVector::ZeroVector;
		}
		const FVector PlanarVelocity(PawnVelocity.X, PawnVelocity.Y, 0.0);
		const double MassScale = CharacterContactMassKg / (CharacterContactMassKg + BodyMassKg);
		return PlanarVelocity.GetClampedToMaxSize(MaxPawnPushVelocity / PawnPushVelocityFraction)
			* (PawnPushVelocityFraction * MassScale);
	}
}

/** Physics Proxy 的稳定碰撞职责；与运动状态、表现和拾取查询相互独立。 */
UENUM()
enum class EWorldObjectPhysicsCollisionPolicy : uint8
{
	Standard,
	LooseDebris
};

/** Physics Actor 的只读形状与质量配置；Transform/速度使用 Actor Movement 通道投影。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectPhysicsBodyNetState final
{
	GENERATED_BODY()

	UPROPERTY()
	bool bConfigured = false;

	UPROPERTY()
	FVector LocalCenter = FVector::ZeroVector;

	UPROPERTY()
	FVector LocalExtent = FVector(1.0f);

	UPROPERTY()
	float MassKg = UE::ElementSandbox::WorldObjects::Physics::DefaultMassKg;

	/** 本次 Physics 生命周期开始时的 WorldEntity StateRevision，用于拒绝迟到的旧代理。 */
	UPROPERTY()
	uint32 ActivationRevision = 0;

	UPROPERTY()
	EWorldObjectPhysicsCollisionPolicy CollisionPolicy =
		EWorldObjectPhysicsCollisionPolicy::Standard;

	bool IsValid() const
	{
		return bConfigured && ActivationRevision != 0
			&& !LocalCenter.ContainsNaN()
			&& !LocalExtent.ContainsNaN()
			&& LocalExtent.X > UE_SMALL_NUMBER
			&& LocalExtent.Y > UE_SMALL_NUMBER
			&& LocalExtent.Z > UE_SMALL_NUMBER
			&& FMath::IsFinite(MassKg)
			&& MassKg > UE_SMALL_NUMBER
			&& CollisionPolicy >= EWorldObjectPhysicsCollisionPolicy::Standard
			&& CollisionPolicy <= EWorldObjectPhysicsCollisionPolicy::LooseDebris;
	}
};

/** Authority 创建 Physics Proxy 时提供的实例初始值。 */
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectPhysicsBodyInit final
{
	float MassKg = UE::ElementSandbox::WorldObjects::Physics::DefaultMassKg;
	EWorldObjectPhysicsCollisionPolicy CollisionPolicy =
		EWorldObjectPhysicsCollisionPolicy::Standard;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocityDegrees = FVector::ZeroVector;

	bool IsValid() const
	{
		return FMath::IsFinite(MassKg)
			&& MassKg > UE_SMALL_NUMBER
			&& CollisionPolicy >= EWorldObjectPhysicsCollisionPolicy::Standard
			&& CollisionPolicy <= EWorldObjectPhysicsCollisionPolicy::LooseDebris
			&& !LinearVelocity.ContainsNaN()
			&& !AngularVelocityDegrees.ContainsNaN();
	}
};

/** ECS 只保存 Physics Proxy 的身份配置，不保存 Chaos 每帧积分结果。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectPhysicsBodyFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	float MassKg = UE::ElementSandbox::WorldObjects::Physics::DefaultMassKg;
	EWorldObjectPhysicsCollisionPolicy CollisionPolicy =
		EWorldObjectPhysicsCollisionPolicy::Standard;
	FVector LocalCollisionCenter = FVector::ZeroVector;
	FVector LocalCollisionExtent = FVector(1.0f);
	FVector InitialLinearVelocity = FVector::ZeroVector;
	FVector InitialAngularVelocityDegrees = FVector::ZeroVector;
};
