#include "Game/BuildingCollisionActivationComponent.h"

#include "BuildingWorldSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"

UBuildingCollisionActivationComponent::UBuildingCollisionActivationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UBuildingCollisionActivationComponent::BeginPlay()
{
	Super::BeginPlay();
	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	SetComponentTickEnabled(
		PlayerController
		&& (PlayerController->IsLocalController()
			|| PlayerController->HasAuthority()));
}

void UBuildingCollisionActivationComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	UpdateMovementTickPrerequisite(nullptr);
	ClearSource();
	Super::EndPlay(EndPlayReason);
}

void UBuildingCollisionActivationComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	UpdateMovementTickPrerequisite(Pawn ? Pawn->GetMovementComponent() : nullptr);
	UWorld* World = GetWorld();
	UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!Pawn || !BuildingSubsystem)
	{
		ClearSource();
		return;
	}

	const FBuildCollisionActivationConfig Config =
		BuildingSubsystem->GetCollisionActivationConfig();
	FBuildCollisionSource NewSource;
	FVector ViewDirection = FVector::ForwardVector;
	if (!TryBuildSource(*Pawn, Config, NewSource, ViewDirection)
		|| !ShouldSubmitSource(*Pawn, NewSource, ViewDirection, Config))
	{
		return;
	}
	NewSource.Revision = NextSourceRevision++;
	if (NextSourceRevision == 0)
	{
		++NextSourceRevision;
	}

	bool bSubmitted = false;
	if (Source.IsSet())
	{
		bSubmitted = BuildingSubsystem->UpdateCollisionSource(Source, NewSource);
		if (!bSubmitted)
		{
			Source = {};
		}
	}
	if (!Source.IsSet())
	{
		Source = BuildingSubsystem->RegisterCollisionSource(NewSource);
		bSubmitted = Source.IsSet();
	}
	if (bSubmitted)
	{
		LastSubmittedPawn = Pawn;
		LastSubmittedSource = NewSource;
		LastSubmittedViewDirection = ViewDirection;
		bHasSubmittedSource = true;
		// Immediate/Camera Body 必须早于本帧 CharacterMovement；普通 Prefetch
		// 仍由 Processor 的 16 Part 预算跨帧推进。
		BuildingSubsystem->FlushCollisionChanges();
	}
}

bool UBuildingCollisionActivationComponent::TryBuildSource(
	const APawn& Pawn,
	const FBuildCollisionActivationConfig& Config,
	FBuildCollisionSource& OutSource,
	FVector& OutViewDirection) const
{
	OutSource = {};
	OutViewDirection = Pawn.GetActorForwardVector().GetSafeNormal();
	if (!Config.IsValid())
	{
		return false;
	}

	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	const FVector SubjectLocation = Pawn.GetActorLocation();
	FVector ViewLocation = SubjectLocation;
	FRotator ViewRotation = Pawn.GetActorRotation();
	if (PlayerController)
	{
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	}
	if (SubjectLocation.ContainsNaN()
		|| ViewLocation.ContainsNaN()
		|| ViewRotation.ContainsNaN())
	{
		return false;
	}
	OutViewDirection = ViewRotation.Vector().GetSafeNormal();

	const FVector BlockingExtent(Config.MinimumBlockingRadius);
	OutSource.SubjectLocation = SubjectLocation;
	OutSource.Velocity = Pawn.GetVelocity();
	OutSource.ImmediateBounds = FBox(
		SubjectLocation - BlockingExtent,
		SubjectLocation + BlockingExtent);
	if (const UCapsuleComponent* Capsule =
		Pawn.FindComponentByClass<UCapsuleComponent>())
	{
		OutSource.ImmediateBounds += Capsule->Bounds.GetBox();
	}

	OutSource.CameraBounds = FBox(ForceInit);
	OutSource.CameraBounds += SubjectLocation;
	OutSource.CameraBounds += ViewLocation;
	OutSource.CameraBounds = OutSource.CameraBounds.ExpandBy(
		Config.MovementSafetyPadding);

	const double Speed = OutSource.Velocity.Size();
	double PredictedDistance = Speed * Config.PredictionHorizonSeconds;
	if (const UCharacterMovementComponent* CharacterMovement =
		Cast<UCharacterMovementComponent>(Pawn.GetMovementComponent()))
	{
		const double BrakingDeceleration =
			CharacterMovement->GetMaxBrakingDeceleration();
		if (BrakingDeceleration > UE_KINDA_SMALL_NUMBER)
		{
			PredictedDistance += FMath::Square(Speed)
				/ (2.0 * BrakingDeceleration);
		}
	}
	const FVector PredictedDisplacement = OutSource.Velocity.GetSafeNormal()
		* PredictedDistance;
	const FBox PredictedImmediateBounds(
		OutSource.ImmediateBounds.Min + PredictedDisplacement,
		OutSource.ImmediateBounds.Max + PredictedDisplacement);
	OutSource.PrefetchBounds = OutSource.ImmediateBounds;
	OutSource.PrefetchBounds += PredictedImmediateBounds;
	OutSource.PrefetchBounds += OutSource.CameraBounds;
	OutSource.RetentionBounds = OutSource.PrefetchBounds.ExpandBy(
		Config.RetentionPadding);
	OutSource.Revision = 1;
	return OutSource.IsValid();
}

bool UBuildingCollisionActivationComponent::ShouldSubmitSource(
	const APawn& Pawn,
	const FBuildCollisionSource& NewSource,
	const FVector& NewViewDirection,
	const FBuildCollisionActivationConfig& Config) const
{
	if (!bHasSubmittedSource || LastSubmittedPawn.Get() != &Pawn)
	{
		return true;
	}
	if (FVector::DistSquared(
			LastSubmittedSource.SubjectLocation,
			NewSource.SubjectLocation)
		>= FMath::Square(Config.SourceMoveThreshold))
	{
		return true;
	}
	if (FVector::Dist(
			LastSubmittedSource.Velocity,
			NewSource.Velocity)
		>= Config.SourceSpeedThreshold)
	{
		return true;
	}
	const double DirectionDot = FMath::Clamp(
		static_cast<double>(FVector::DotProduct(
			LastSubmittedViewDirection,
			NewViewDirection)),
		-1.0,
		1.0);
	return FMath::RadiansToDegrees(FMath::Acos(DirectionDot))
		>= Config.SourceDirectionThresholdDegrees;
}

void UBuildingCollisionActivationComponent::UpdateMovementTickPrerequisite(
	UMovementComponent* NewMovementComponent)
{
	if (PrerequisiteMovementComponent.Get() == NewMovementComponent)
	{
		return;
	}

	if (UMovementComponent* Previous = PrerequisiteMovementComponent.Get())
	{
		Previous->PrimaryComponentTick.RemovePrerequisite(
			this,
			PrimaryComponentTick);
	}
	PrerequisiteMovementComponent = NewMovementComponent;
	if (NewMovementComponent)
	{
		NewMovementComponent->PrimaryComponentTick.AddPrerequisite(
			this,
			PrimaryComponentTick);
	}
}

void UBuildingCollisionActivationComponent::ClearSource()
{
	if (Source.IsSet())
	{
		if (UWorld* World = GetWorld())
		{
			if (UBuildingWorldSubsystem* BuildingSubsystem =
				World->GetSubsystem<UBuildingWorldSubsystem>())
			{
				BuildingSubsystem->UnregisterCollisionSource(Source);
			}
		}
	}
	Source = {};
	LastSubmittedSource = {};
	LastSubmittedViewDirection = FVector::ForwardVector;
	LastSubmittedPawn.Reset();
	bHasSubmittedSource = false;
}
