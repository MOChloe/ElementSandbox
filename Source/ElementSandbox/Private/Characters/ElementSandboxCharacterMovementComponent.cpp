#include "Characters/ElementSandboxCharacterMovementComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"

void UElementSandboxCharacterMovementComponent::PerformMovement(const float DeltaSeconds)
{
	PredictDebrisContacts(DeltaSeconds);
	Super::PerformMovement(DeltaSeconds);
}

void UElementSandboxCharacterMovementComponent::PredictDebrisContacts(const float DeltaSeconds)
{
	if (!HasValidData() || !UpdatedPrimitive || !IsMovingOnGround() || Velocity.SizeSquared2D() < 1.0) return;
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.0);
	// Server 一帧可重放多个 Client Move，刚体位移却要等下一次 Chaos 步才生效。
	// 提前两个当前物理帧检查少量 Awake Body，避免每次追上木块都先把角色速度截为零。
	const double LeadSeconds = FMath::Min(0.1, 2.0 * FMath::Max(double(DeltaSeconds), double(GetWorld()->GetDeltaSeconds())));
	const FVector Start = UpdatedPrimitive->GetComponentLocation();
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CharacterDebrisContact), false, GetOwner());
	DebrisContactScratch.Reset();
	GetWorld()->SweepMultiByObjectType(DebrisContactScratch, Start, Start + PlanarVelocity * LeadSeconds,
		UpdatedPrimitive->GetComponentQuat(), FCollisionObjectQueryParams(ECC_PhysicsBody),
		UpdatedPrimitive->GetCollisionShape(), Query);
	for (const FHitResult& Hit : DebrisContactScratch)
	{
		const auto* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Hit.GetActor());
		if (PhysicsProxy && PhysicsProxy->GetConfiguredCollisionPolicy() == EWorldObjectPhysicsCollisionPolicy::LooseDebris)
			ApplyImpactPhysicsForces(Hit, Acceleration, PlanarVelocity);
	}
}

void UElementSandboxCharacterMovementComponent::ApplyImpactPhysicsForces(
	const FHitResult& Impact,
	const FVector& ImpactAcceleration,
	const FVector& ImpactVelocity)
{
	const AWorldObjectPhysicsProxyActor* PhysicsProxy =
		Cast<AWorldObjectPhysicsProxyActor>(Impact.GetActor());
	if (!PhysicsProxy
		|| PhysicsProxy->GetConfiguredCollisionPolicy()
			!= EWorldObjectPhysicsCollisionPolicy::LooseDebris)
	{
		Super::ApplyImpactPhysicsForces(Impact, ImpactAcceleration, ImpactVelocity);
		return;
	}

	UPrimitiveComponent* ImpactComponent = Impact.GetComponent();
	if (!ImpactComponent || !ImpactComponent->IsSimulatingPhysics(Impact.BoneName))
	{
		return;
	}
	const float BodyMassKg = ImpactComponent->GetMass();

	const FVector DesiredVelocity =
		UE::ElementSandbox::WorldObjects::Physics::ComputePawnPushVelocity(
			ImpactVelocity, BodyMassKg);
	const double DesiredSpeed = DesiredVelocity.Size2D();
	if (DesiredSpeed <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector Direction = DesiredVelocity / DesiredSpeed;
	const FVector CurrentVelocity = ImpactComponent->GetPhysicsLinearVelocity();
	const double CurrentSpeedAlongPush = FVector::DotProduct(CurrentVelocity, Direction);
	const double MissingSpeed = DesiredSpeed - CurrentSpeedAlongPush;
	if (MissingSpeed > UE_KINDA_SMALL_NUMBER)
	{
		// 同一个 Chaos 步前可处理多个 Client Move/命中。AddImpulse 会累积尚未积分的冲量，
		// 读到的旧速度无法防止重复加速；直接补齐当前速度才能保持一次或多次接触结果相同。
		ImpactComponent->SetPhysicsLinearVelocity(CurrentVelocity + Direction * MissingSpeed, false, Impact.BoneName);
	}
}
