#include "Game/WorldObjectCollisionActivationComponent.h"

#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

UWorldObjectCollisionActivationComponent::UWorldObjectCollisionActivationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UWorldObjectCollisionActivationComponent::BeginPlay()
{
	Super::BeginPlay();
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	SetComponentTickEnabled(Controller && (Controller->IsLocalController() || Controller->HasAuthority()));
}

void UWorldObjectCollisionActivationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UpdatePawnHitBinding(nullptr);
	UpdateMovementTickPrerequisite(nullptr);
	ClearSource();
	Super::EndPlay(EndPlayReason);
}

void UWorldObjectCollisionActivationComponent::TickComponent(
	const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	(void)DeltaTime;
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	UpdatePawnHitBinding(Pawn);
	UpdateMovementTickPrerequisite(Pawn ? Pawn->GetMovementComponent() : nullptr);
	UWorldObjectCollisionWorldSubsystem* Collision = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectCollisionWorldSubsystem>() : nullptr;
	if (!Pawn || !Collision)
	{
		ClearSource();
		return;
	}

	const FWorldObjectCollisionActivationConfig& Config = Collision->GetActivationConfig();
	FWorldObjectCollisionSource NewSource;
	FVector ViewDirection = FVector::ForwardVector;
	if (!TryBuildSource(*Pawn, Config, NewSource, ViewDirection)
		|| !ShouldSubmitSource(*Pawn, NewSource, ViewDirection, Config))
	{
		return;
	}
	NewSource.Revision = NextSourceRevision++;
	if (NextSourceRevision == 0) ++NextSourceRevision;

	bool bSubmitted = Source.IsSet() && Collision->UpdateSource(Source, NewSource);
	if (!bSubmitted)
	{
		Source = Collision->RegisterSource(NewSource);
		bSubmitted = Source.IsSet();
	}
	if (bSubmitted)
	{
		LastSubmittedPawn = Pawn;
		LastSubmittedSource = NewSource;
		LastSubmittedViewDirection = ViewDirection;
		bHasSubmittedSource = true;
		Collision->FlushImmediateCollisionChanges();
	}
}

void UWorldObjectCollisionActivationComponent::UpdatePawnHitBinding(APawn* NewPawn)
{
	UPrimitiveComponent* NewCollisionComponent = nullptr;
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (Controller && Controller->HasAuthority() && NewPawn)
	{
		NewCollisionComponent = NewPawn->FindComponentByClass<UCapsuleComponent>();
	}
	if (BoundPawnCollisionComponent.Get() == NewCollisionComponent) return;
	if (UPrimitiveComponent* Previous = BoundPawnCollisionComponent.Get())
	{
		Previous->OnComponentHit.RemoveDynamic(
			this, &UWorldObjectCollisionActivationComponent::HandlePawnHit);
	}
	BoundPawnCollisionComponent = NewCollisionComponent;
	if (NewCollisionComponent)
	{
		NewCollisionComponent->OnComponentHit.AddUniqueDynamic(
			this, &UWorldObjectCollisionActivationComponent::HandlePawnHit);
	}
}

void UWorldObjectCollisionActivationComponent::HandlePawnHit(
	UPrimitiveComponent* HitComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, const FVector NormalImpulse, const FHitResult& Hit)
{
	(void)OtherActor;
	(void)NormalImpulse;
	APawn* Pawn = Cast<APawn>(HitComponent ? HitComponent->GetOwner() : nullptr);
	UWorldObjectCollisionWorldSubsystem* Collision = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectCollisionWorldSubsystem>() : nullptr;
	if (Pawn && OtherComponent && Collision && Hit.Item != INDEX_NONE)
	{
		Collision->QueueLooseDebrisPawnContact(
			*OtherComponent, Hit.Item, Pawn->GetVelocity());
	}
}

bool UWorldObjectCollisionActivationComponent::TryBuildSource(
	const APawn& Pawn, const FWorldObjectCollisionActivationConfig& Config,
	FWorldObjectCollisionSource& OutSource, FVector& OutViewDirection) const
{
	OutSource = {};
	if (!Config.IsValid()) return false;
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	const FVector SubjectLocation = Pawn.GetActorLocation();
	FVector ViewLocation = SubjectLocation;
	FRotator ViewRotation = Pawn.GetActorRotation();
	if (Controller) Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
	if (SubjectLocation.ContainsNaN() || ViewLocation.ContainsNaN() || ViewRotation.ContainsNaN()) return false;
	OutViewDirection = ViewRotation.Vector().GetSafeNormal();
	if (OutViewDirection.IsNearlyZero()) OutViewDirection = Pawn.GetActorForwardVector().GetSafeNormal();

	const double Safety = Config.SourceMoveThreshold;
	const FVector ImmediateExtent(Config.ImmediateCollisionRadius + Safety);
	OutSource.SubjectLocation = SubjectLocation;
	OutSource.ViewLocation = ViewLocation;
	OutSource.ViewDirection = OutViewDirection;
	OutSource.Velocity = Pawn.GetVelocity();
	FBox PawnBounds(
		SubjectLocation - FVector(50.0),
		SubjectLocation + FVector(50.0));
	OutSource.ImmediateBounds = FBox(SubjectLocation - ImmediateExtent, SubjectLocation + ImmediateExtent);
	if (const UCapsuleComponent* Capsule = Pawn.FindComponentByClass<UCapsuleComponent>())
	{
		PawnBounds = Capsule->Bounds.GetBox();
		OutSource.ImmediateBounds += PawnBounds;
	}
	const FVector ContactDisplacement = OutSource.Velocity * Config.PawnContactPredictionSeconds;
	OutSource.PawnContactBounds = PawnBounds.ExpandBy(Config.PawnContactPadding);
	OutSource.PawnContactBounds += FBox(
		PawnBounds.Min + ContactDisplacement,
		PawnBounds.Max + ContactDisplacement).ExpandBy(Config.PawnContactPadding);
	FBox CameraCorridor(ForceInit);
	CameraCorridor += SubjectLocation;
	CameraCorridor += ViewLocation;
	CameraCorridor = CameraCorridor.ExpandBy(Config.CameraCorridorPadding + Safety);

	const double Speed = OutSource.Velocity.Size();
	double PredictionDistance = Speed * Config.PredictionSeconds;
	if (const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(Pawn.GetMovementComponent()))
	{
		const double Deceleration = Movement->GetMaxBrakingDeceleration();
		if (Deceleration > UE_KINDA_SMALL_NUMBER)
		{
			PredictionDistance += FMath::Square(Speed) / (2.0 * Deceleration);
		}
	}
	const FVector Displacement = OutSource.Velocity.GetSafeNormal() * PredictionDistance;
	OutSource.PrefetchBounds = OutSource.ImmediateBounds;
	OutSource.PrefetchBounds += FBox(
		OutSource.ImmediateBounds.Min + Displacement,
		OutSource.ImmediateBounds.Max + Displacement);
	OutSource.PrefetchBounds += CameraCorridor;
	OutSource.RetentionBounds = OutSource.PrefetchBounds.ExpandBy(Config.RetentionPadding);
	OutSource.Revision = 1;
	return OutSource.IsValid();
}

bool UWorldObjectCollisionActivationComponent::ShouldSubmitSource(
	const APawn& Pawn, const FWorldObjectCollisionSource& NewSource,
	const FVector& NewViewDirection, const FWorldObjectCollisionActivationConfig& Config) const
{
	if (!bHasSubmittedSource || LastSubmittedPawn.Get() != &Pawn) return true;
	if (FVector::DistSquared(LastSubmittedSource.SubjectLocation, NewSource.SubjectLocation)
		>= FMath::Square(Config.SourceMoveThreshold)) return true;
	if (FVector::DistSquared(LastSubmittedSource.ViewLocation, NewSource.ViewLocation)
		>= FMath::Square(Config.SourceMoveThreshold)) return true;
	if (FVector::DistSquared(LastSubmittedSource.Velocity, NewSource.Velocity)
		>= FMath::Square(Config.SourceSpeedThreshold)) return true;
	const double Dot = FMath::Clamp(static_cast<double>(FVector::DotProduct(
		LastSubmittedViewDirection, NewViewDirection)), -1.0, 1.0);
	return FMath::RadiansToDegrees(FMath::Acos(Dot)) >= Config.SourceDirectionThresholdDegrees;
}

void UWorldObjectCollisionActivationComponent::UpdateMovementTickPrerequisite(UMovementComponent* NewMovementComponent)
{
	if (PrerequisiteMovementComponent.Get() == NewMovementComponent) return;
	if (UMovementComponent* Previous = PrerequisiteMovementComponent.Get())
	{
		Previous->PrimaryComponentTick.RemovePrerequisite(this, PrimaryComponentTick);
	}
	PrerequisiteMovementComponent = NewMovementComponent;
	if (NewMovementComponent)
	{
		NewMovementComponent->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}
}

void UWorldObjectCollisionActivationComponent::ClearSource()
{
	if (Source.IsSet() && GetWorld())
	{
		if (UWorldObjectCollisionWorldSubsystem* Collision =
			GetWorld()->GetSubsystem<UWorldObjectCollisionWorldSubsystem>())
		{
			Collision->UnregisterSource(Source);
		}
	}
	Source = {};
	LastSubmittedSource = {};
	LastSubmittedViewDirection = FVector::ForwardVector;
	LastSubmittedPawn.Reset();
	bHasSubmittedSource = false;
}
