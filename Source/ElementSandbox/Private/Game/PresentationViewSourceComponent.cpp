#include "Game/PresentationViewSourceComponent.h"

#include "PresentationWorldSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"

UPresentationViewSourceComponent::UPresentationViewSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UPresentationViewSourceComponent::BeginPlay()
{
	Super::BeginPlay();
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	SetComponentTickEnabled(
		Controller && Controller->IsLocalController() && GetNetMode() != NM_DedicatedServer);
}

void UPresentationViewSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SourceHandle.IsSet())
	{
		if (UWorld* World = GetWorld())
		{
			if (UPresentationWorldSubsystem* Presentation =
				World->GetSubsystem<UPresentationWorldSubsystem>())
			{
				Presentation->UnregisterSource(SourceHandle);
			}
		}
		SourceHandle = {};
	}
	Super::EndPlay(EndPlayReason);
}

void UPresentationViewSourceComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	FPresentationViewSource Source;
	if (!TryBuildViewSource(Source))
	{
		return;
	}
	UPresentationWorldSubsystem* Presentation = GetWorld()
		? GetWorld()->GetSubsystem<UPresentationWorldSubsystem>()
		: nullptr;
	if (!Presentation)
	{
		return;
	}
	if (!SourceHandle.IsSet())
	{
		SourceHandle = Presentation->RegisterSource(Source);
		if (SourceHandle.IsSet())
		{
			RememberPublishedView(Source);
		}
	}
	else if (Presentation->UpdateSource(SourceHandle, Source))
	{
		RememberPublishedView(Source);
	}
	else
	{
		SourceHandle = Presentation->RegisterSource(Source);
		if (SourceHandle.IsSet())
		{
			RememberPublishedView(Source);
		}
	}
}

bool UPresentationViewSourceComponent::TryBuildViewSource(
	FPresentationViewSource& OutSource)
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		return false;
	}
	FVector Location;
	FRotator Rotation;
	Controller->GetPlayerViewPoint(Location, Rotation);
	int32 Width = 0;
	int32 Height = 0;
	Controller->GetViewportSize(Width, Height);
	if (Width <= 0 || Height <= 0)
	{
		return false;
	}

	OutSource.ViewLocation = Location;
	OutSource.SubjectLocation = IsValid(Controller->GetPawn())
		? Controller->GetPawn()->GetActorLocation()
		: Location;
	OutSource.Forward = Rotation.Vector();
	OutSource.Right = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Y);
	OutSource.Up = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Z);
	OutSource.HorizontalFOVDegrees = Controller->PlayerCameraManager
		? Controller->PlayerCameraManager->GetFOVAngle()
		: 90.0f;
	OutSource.AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
	OutSource.ViewportSize = FIntPoint(Width, Height);
	OutSource.Priority = 0;
	if (bHasPublishedView
		&& LastViewLocation.Equals(OutSource.ViewLocation, 0.1)
		&& LastSubjectLocation.Equals(OutSource.SubjectLocation, 0.1)
		&& LastForward.Equals(OutSource.Forward, 1.0e-4)
		&& LastRight.Equals(OutSource.Right, 1.0e-4)
		&& LastUp.Equals(OutSource.Up, 1.0e-4)
		&& FMath::IsNearlyEqual(LastHorizontalFOVDegrees, OutSource.HorizontalFOVDegrees, 0.01f)
		&& FMath::IsNearlyEqual(LastAspectRatio, OutSource.AspectRatio, 1.0e-4f)
		&& LastViewportSize == OutSource.ViewportSize)
	{
		// 静止相机/角色只做一次轻量采样，不构建 Frustum、不广播 Source、不唤醒 Projector。
		return false;
	}
	OutSource.Revision = NextSourceRevision++;
	if (NextSourceRevision == 0)
	{
		NextSourceRevision = 1;
	}

	const ULocalPlayer* LocalPlayer = Controller->GetLocalPlayer();
	FSceneViewProjectionData ProjectionData;
	if (LocalPlayer && LocalPlayer->ViewportClient
		&& LocalPlayer->GetProjectionData(
			LocalPlayer->ViewportClient->Viewport,
			ProjectionData))
	{
		GetViewFrustumBounds(
			OutSource.ViewFrustum,
			ProjectionData.ComputeViewProjectionMatrix(),
			/*bUseNearPlane*/ true);
	}
	return OutSource.IsValid();
}

void UPresentationViewSourceComponent::RememberPublishedView(
	const FPresentationViewSource& Source)
{
	LastViewLocation = Source.ViewLocation;
	LastSubjectLocation = Source.SubjectLocation;
	LastForward = Source.Forward;
	LastRight = Source.Right;
	LastUp = Source.Up;
	LastHorizontalFOVDegrees = Source.HorizontalFOVDegrees;
	LastAspectRatio = Source.AspectRatio;
	LastViewportSize = Source.ViewportSize;
	bHasPublishedView = true;
}
