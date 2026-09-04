#include "Tree/SettlementTreeCollisionSourceComponent.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Tree/SettlementTreeCollisionWorldSubsystem.h"
#include "Tree/SettlementTreeSettings.h"

USettlementTreeCollisionSourceComponent::USettlementTreeCollisionSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USettlementTreeCollisionSourceComponent::BeginPlay()
{
	Super::BeginPlay();
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	SetComponentTickEnabled(Controller && (Controller->IsLocalController() || Controller->HasAuthority()));
}

void USettlementTreeCollisionSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UpdateMovementTickPrerequisite(nullptr);
	ClearSource();
	Super::EndPlay(EndPlayReason);
}

void USettlementTreeCollisionSourceComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	(void)DeltaTime;
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	UpdateMovementTickPrerequisite(Pawn ? Pawn->GetMovementComponent() : nullptr);
	USettlementTreeCollisionWorldSubsystem* Collision = GetWorld()
		? GetWorld()->GetSubsystem<USettlementTreeCollisionWorldSubsystem>()
		: nullptr;
	if (!Pawn || !Collision)
	{
		ClearSource();
		return;
	}
	const USettlementTreeSettings* Settings = GetDefault<USettlementTreeSettings>();
	FSettlementTreeCollisionSource Source;
	Source.SubjectLocation = Pawn->GetActorLocation();
	Source.Velocity = Pawn->GetVelocity();
	FVector ViewLocation = Source.SubjectLocation;
	FRotator ViewRotation = Pawn->GetActorRotation();
	Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
	Source.ViewLocation = ViewLocation;
	Source.ViewDirection = FVector(
		ViewRotation.Vector().X,
		ViewRotation.Vector().Y,
		0.0).GetSafeNormal();
	if (Source.ViewDirection.IsNearlyZero())
	{
		Source.ViewDirection = Pawn->GetActorForwardVector().GetSafeNormal2D();
	}

	const bool bPawnChanged = LastSubmittedPawn.Get() != Pawn;
	const bool bMoved = !bHasSubmittedSource
		|| FVector::DistSquared(Source.SubjectLocation, LastSubmittedSource.SubjectLocation)
			>= FMath::Square(Settings->CollisionSourceMoveThreshold)
		|| FVector::DistSquared(Source.ViewLocation, LastSubmittedSource.ViewLocation)
			>= FMath::Square(Settings->CollisionSourceMoveThreshold);
	const bool bVelocityChanged = !bHasSubmittedSource
		|| FVector::DistSquared(Source.Velocity, LastSubmittedSource.Velocity)
			>= FMath::Square(Settings->CollisionSourceSpeedThreshold);
	const bool bDirectionChanged = !bHasSubmittedSource
		|| FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			static_cast<double>(FVector::DotProduct(
				Source.ViewDirection.GetSafeNormal(),
				LastSubmittedSource.ViewDirection.GetSafeNormal())),
			-1.0,
			1.0))) >= Settings->CollisionSourceDirectionThresholdDegrees;
	if (!bPawnChanged && !bMoved && !bVelocityChanged && !bDirectionChanged)
	{
		return;
	}

	// 查询 Bounds 预留一个 Source 位移阈值；阈值内不重查时 4m Immediate 和相机走廊仍被覆盖。
	const double SourceSafety = Settings->CollisionSourceMoveThreshold;
	const FVector ImmediateExtent(Settings->ImmediateCollisionRadius + SourceSafety);
	Source.ImmediateBounds = FBox(Source.SubjectLocation - ImmediateExtent, Source.SubjectLocation + ImmediateExtent);
	if (const UCapsuleComponent* Capsule = Pawn->FindComponentByClass<UCapsuleComponent>())
	{
		Source.ImmediateBounds += Capsule->Bounds.GetBox();
	}
	FBox CameraCorridor(ForceInit);
	CameraCorridor += Source.SubjectLocation;
	CameraCorridor += ViewLocation;
	CameraCorridor = CameraCorridor.ExpandBy(Settings->CameraCorridorPadding + SourceSafety);
	const double Speed = Source.Velocity.Size();
	double PredictionDistance = Speed * Settings->PredictionSeconds;
	if (const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(Pawn->GetMovementComponent()))
	{
		const double Deceleration = Movement->GetMaxBrakingDeceleration();
		if (Deceleration > UE_KINDA_SMALL_NUMBER)
		{
			PredictionDistance += FMath::Square(Speed) / (2.0 * Deceleration);
		}
	}
	const FVector Displacement = Source.Velocity.GetSafeNormal() * PredictionDistance;
	Source.PrefetchBounds = Source.ImmediateBounds;
	Source.PrefetchBounds += FBox(
		Source.ImmediateBounds.Min + Displacement,
		Source.ImmediateBounds.Max + Displacement);
	Source.PrefetchBounds += CameraCorridor;
	Source.RetentionBounds = Source.PrefetchBounds.ExpandBy(Settings->RetentionPadding);
	Source.Revision = NextRevision++;
	if (NextRevision == 0)
	{
		++NextRevision;
	}
	bool bSubmitted = SourceHandle.IsSet() && Collision->UpdateSource(SourceHandle, Source);
	if (!bSubmitted)
	{
		SourceHandle = Collision->RegisterSource(Source);
		bSubmitted = SourceHandle.IsSet();
	}
	if (bSubmitted)
	{
		LastSubmittedPawn = Pawn;
		LastSubmittedSource = Source;
		bHasSubmittedSource = true;
		Collision->FlushImmediateCollisionChanges();
	}
}

void USettlementTreeCollisionSourceComponent::UpdateMovementTickPrerequisite(
	UMovementComponent* MovementComponent)
{
	if (PrerequisiteMovementComponent.Get() == MovementComponent)
	{
		return;
	}
	if (UMovementComponent* Previous = PrerequisiteMovementComponent.Get())
	{
		Previous->PrimaryComponentTick.RemovePrerequisite(this, PrimaryComponentTick);
	}
	PrerequisiteMovementComponent = MovementComponent;
	if (MovementComponent)
	{
		MovementComponent->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}
}

void USettlementTreeCollisionSourceComponent::ClearSource()
{
	if (SourceHandle.IsSet() && GetWorld())
	{
		if (USettlementTreeCollisionWorldSubsystem* Collision =
			GetWorld()->GetSubsystem<USettlementTreeCollisionWorldSubsystem>())
		{
			Collision->UnregisterSource(SourceHandle);
		}
	}
	SourceHandle = {};
	LastSubmittedPawn.Reset();
	LastSubmittedSource = {};
	bHasSubmittedSource = false;
}
