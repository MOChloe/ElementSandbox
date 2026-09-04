#include "Projection/WorldObjectProxyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "WorldObjectWorldSubsystem.h"

UWorldObjectProxyComponent::UWorldObjectProxyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UWorldObjectProxyComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWorldObjectProxyComponent, WorldEntityId);
	DOREPLIFETIME(UWorldObjectProxyComponent, bPhysicsProjectionActive);
}

void UWorldObjectProxyComponent::BeginPlay()
{
	Super::BeginPlay();
	BindPhysicsDelegates();
	RegisterWithSubsystem();
}

void UWorldObjectProxyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindPhysicsDelegates();
	if (WorldEntityId.IsSet())
	{
		if (UWorldObjectWorldSubsystem* Subsystem = GetWorld()
			? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>()
			: nullptr)
		{
			Subsystem->NotifyProxyEndPlay(
				WorldEntityId,
				*this,
				bDestroyEntityOnEndPlay && GetOwner() && GetOwner()->HasAuthority());
		}
	}
	LocalEntity = {};
	Super::EndPlay(EndPlayReason);
}

UPrimitiveComponent* UWorldObjectProxyComponent::GetPhysicsPrimitive() const
{
	return GetOwner() ? Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent()) : nullptr;
}

bool UWorldObjectProxyComponent::AssignAuthorityWorldEntityId(const FWorldEntityId InWorldEntityId)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !InWorldEntityId.IsSet()
		|| (WorldEntityId.IsSet() && WorldEntityId != InWorldEntityId))
	{
		return false;
	}
	WorldEntityId = InWorldEntityId;
	RegisterWithSubsystem();
	GetOwner()->ForceNetUpdate();
	return true;
}

bool UWorldObjectProxyComponent::SetAuthorityPhysicsProjectionActive(const bool bActive)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || !GetPhysicsPrimitive())
	{
		return false;
	}
	if (bPhysicsProjectionActive == bActive)
	{
		ApplyPhysicsProjectionState();
		return true;
	}
	bPhysicsProjectionActive = bActive;
	ApplyPhysicsProjectionState();
	Owner->ForceNetUpdate();
	return true;
}

void UWorldObjectProxyComponent::OnRep_WorldEntityId()
{
	RegisterWithSubsystem();
}

void UWorldObjectProxyComponent::OnRep_PhysicsProjectionActive()
{
	ApplyPhysicsProjectionState();
}

void UWorldObjectProxyComponent::ApplyPhysicsProjectionState()
{
	AActor* Owner = GetOwner();
	UPrimitiveComponent* Primitive = GetPhysicsPrimitive();
	if (!Owner || !Primitive)
	{
		return;
	}
	if (bPhysicsProjectionActive)
	{
		Owner->SetActorEnableCollision(true);
		Primitive->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
		Primitive->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Primitive->SetSimulatePhysics(true);
	}
	else
	{
		Primitive->SetSimulatePhysics(false);
		Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Owner->SetActorEnableCollision(false);
	}
}

void UWorldObjectProxyComponent::HandleComponentWake(
	UPrimitiveComponent* WakingComponent,
	const FName BoneName)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !WorldEntityId.IsSet())
	{
		return;
	}
	GetOwner()->FlushNetDormancy();
	GetOwner()->SetNetDormancy(DORM_Awake);
	if (UWorldObjectWorldSubsystem* Subsystem = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr)
	{
		Subsystem->QueueProxyMotionState(WorldEntityId, EWorldObjectMotionState::Physics);
	}
	GetOwner()->SetReplicateMovement(true);
	GetOwner()->ForceNetUpdate();
}

void UWorldObjectProxyComponent::HandleComponentSleep(
	UPrimitiveComponent* SleepingComponent,
	const FName BoneName)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !WorldEntityId.IsSet())
	{
		return;
	}
	if (UWorldObjectWorldSubsystem* Subsystem = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr)
	{
		Subsystem->QueueProxyMotionState(WorldEntityId, EWorldObjectMotionState::Dormant);
	}
	GetOwner()->ForceNetUpdate();
}

void UWorldObjectProxyComponent::RegisterWithSubsystem()
{
	if (!WorldEntityId.IsSet())
	{
		return;
	}
	if (UWorldObjectWorldSubsystem* Subsystem = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr)
	{
		Subsystem->RegisterProxy(*this);
	}
}

void UWorldObjectProxyComponent::BindPhysicsDelegates()
{
	if (UPrimitiveComponent* Primitive = GetPhysicsPrimitive())
	{
		Primitive->OnComponentWake.AddUniqueDynamic(
			this,
			&UWorldObjectProxyComponent::HandleComponentWake);
		Primitive->OnComponentSleep.AddUniqueDynamic(
			this,
			&UWorldObjectProxyComponent::HandleComponentSleep);
	}
}

void UWorldObjectProxyComponent::UnbindPhysicsDelegates()
{
	if (UPrimitiveComponent* Primitive = GetPhysicsPrimitive())
	{
		Primitive->OnComponentWake.RemoveDynamic(
			this,
			&UWorldObjectProxyComponent::HandleComponentWake);
		Primitive->OnComponentSleep.RemoveDynamic(
			this,
			&UWorldObjectProxyComponent::HandleComponentSleep);
	}
}
