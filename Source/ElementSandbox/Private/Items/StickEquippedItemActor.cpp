#include "Items/StickEquippedItemActor.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr double StickHalfHeight = 37.5;
	constexpr double StickRadius = 3.0;
	constexpr double GripToCenterOffset = 28.0;
}

AStickEquippedItemActor::AStickEquippedItemActor()
{
	PhysicsRoot = CreateDefaultSubobject<UCapsuleComponent>(TEXT("PhysicsRoot"));
	SetRootComponent(PhysicsRoot);
	SceneRoot->SetupAttachment(PhysicsRoot);
	PhysicsRoot->InitCapsuleSize(StickRadius, StickHalfHeight);
	PhysicsRoot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PhysicsRoot->SetGenerateOverlapEvents(false);
	PhysicsRoot->SetCanEverAffectNavigation(false);
	PhysicsRoot->SetCastShadow(false);
	PhysicsRoot->bCastDynamicShadow = false;
	PhysicsRoot->bCastStaticShadow = false;
	PhysicsRoot->BodyInstance.bGenerateWakeEvents = true;

	WorldObjectProxyComponent = CreateDefaultSubobject<UWorldObjectProxyComponent>(
		TEXT("WorldObjectProxyComponent"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		GetItemMesh()->SetStaticMesh(CylinderMesh.Object);
	}

	GetItemMesh()->SetRelativeScale3D(FVector(0.05, 0.05, 0.75));
	GetItemMesh()->SetRelativeLocation(FVector::ZeroVector);

	SetNetUpdateFrequency(30.0f);
}

void AStickEquippedItemActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureFirePresentation();
}

void AStickEquippedItemActor::EnsureFirePresentation()
{
	if (GetNetMode() == NM_DedicatedServer || LowerFlame || UpperFlame) return;
	UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UMaterialInterface* FlameMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	const auto CreateFlame = [this, ConeMesh, FlameMaterial](
		const FName Name,
		const FVector& Location,
		const FVector& Scale)
	{
		UStaticMeshComponent* Flame = NewObject<UStaticMeshComponent>(this, Name);
		AddInstanceComponent(Flame);
		Flame->SetupAttachment(SceneRoot);
		Flame->SetRelativeLocation(Location);
		Flame->SetRelativeScale3D(Scale);
		Flame->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Flame->SetGenerateOverlapEvents(false);
		Flame->SetCanEverAffectNavigation(false);
		Flame->SetCastShadow(false);
		Flame->bCastDynamicShadow = false;
		Flame->bCastStaticShadow = false;
		Flame->SetHiddenInGame(true);
		Flame->SetVisibility(false);
		Flame->SetStaticMesh(ConeMesh);
		if (FlameMaterial) Flame->SetMaterial(0, FlameMaterial);
		Flame->RegisterComponent();
		return Flame;
	};
	LowerFlame = CreateFlame(
		TEXT("LowerFlame"), FVector(0.0, 0.0, 36.0), FVector(0.10, 0.10, 0.16));
	UpperFlame = CreateFlame(
		TEXT("UpperFlame"), FVector(0.0, 0.0, 43.0), FVector(0.065, 0.065, 0.12));
	UpdateBurningVisual();
}

void AStickEquippedItemActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AStickEquippedItemActor, bBurning);
}

void AStickEquippedItemActor::ApplyHeldGripOffset()
{
	if (!HasAuthority()
		|| bHeldGripOffsetApplied
		|| !GetRootComponent()
		|| !GetRootComponent()->GetAttachParent())
	{
		return;
	}

	AddActorWorldOffset(
		GetActorQuat().RotateVector(FVector(0.0, 0.0, GripToCenterOffset)),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	bHeldGripOffsetApplied = true;
	ForceNetUpdate();
}

bool AStickEquippedItemActor::CanBeginServerThrow() const
{
	return HasAuthority()
		&& IsValid(PhysicsRoot)
		&& !PhysicsRoot->IsSimulatingPhysics()
		&& GetRootComponent()
		&& GetRootComponent()->GetAttachParent();
}

bool AStickEquippedItemActor::BeginServerThrow(const FVector& InitialVelocity)
{
	if (!CanBeginServerThrow() || InitialVelocity.ContainsNaN())
	{
		return false;
	}

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetOwner(nullptr);
	SetReplicateMovement(true);
	PhysicsRoot->SetMassOverrideInKg(NAME_None, 0.5f, true);
	if (!WorldObjectProxyComponent->SetAuthorityPhysicsProjectionActive(true))
	{
		return false;
	}
	PhysicsRoot->SetPhysicsLinearVelocity(InitialVelocity);
	PhysicsRoot->WakeAllRigidBodies();
	ForceNetUpdate();
	return PhysicsRoot->IsSimulatingPhysics();
}

void AStickEquippedItemActor::QueryElementFireContext(
	bool& bOutEquipped,
	ACharacter*& OutHolderCharacter) const
{
	OutHolderCharacter = Cast<ACharacter>(GetOwner());
	bOutEquipped = IsValid(OutHolderCharacter)
		&& GetRootComponent()
		&& GetRootComponent()->GetAttachParent()
		&& !PhysicsRoot->IsSimulatingPhysics();
}

void AStickEquippedItemActor::SetBurning(const bool bNewBurning)
{
	if (!HasAuthority() || bBurning == bNewBurning)
	{
		return;
	}
	bBurning = bNewBurning;
	UpdateBurningVisual();
	ForceNetUpdate();
}

void AStickEquippedItemActor::OnRep_Burning()
{
	UpdateBurningVisual();
}

void AStickEquippedItemActor::UpdateBurningVisual()
{
	for (UStaticMeshComponent* Flame : {LowerFlame.Get(), UpperFlame.Get()})
	{
		if (IsValid(Flame))
		{
			Flame->SetHiddenInGame(!bBurning, true);
			Flame->SetVisibility(bBurning, true);
		}
	}
}
