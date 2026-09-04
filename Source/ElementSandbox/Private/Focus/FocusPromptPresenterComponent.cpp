#include "Focus/FocusPromptPresenterComponent.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Interaction/InteractionPromptWidget.h"
#include "WorldObjects/WorldObjectPickupComponent.h"

UFocusPromptPresenterComponent::UFocusPromptPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UFocusPromptPresenterComponent::BeginPlay()
{
	Super::BeginPlay();

	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	const bool bCanRunLocally = PlayerController
		&& PlayerController->IsLocalController()
		&& GetNetMode() != NM_DedicatedServer
		&& !IsRunningCommandlet();
	SetComponentTickEnabled(bCanRunLocally);
	if (!bCanRunLocally)
	{
		return;
	}

	if (UFocusHostComponent* FocusHost =
		PlayerController->FindComponentByClass<UFocusHostComponent>())
	{
		AddTickPrerequisiteComponent(FocusHost);
	}
	EnsurePromptWidget();
}

void UFocusPromptPresenterComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (PromptWidget)
	{
		PromptWidget->RemoveFromParent();
	}
	PromptWidget = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UFocusPromptPresenterComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	UFocusHostComponent* FocusHost = PlayerController
		? PlayerController->FindComponentByClass<UFocusHostComponent>()
		: nullptr;
	FFocusInteractionPrompt Prompt;
	if (!PlayerController
		|| !PlayerController->CanUseFocusInteraction()
		|| !FocusHost)
	{
		HidePrompt();
		return;
	}
	const auto* PickupInput = PlayerController->FindComponentByClass<UWorldObjectPickupComponent>();
	if (!(PickupInput && PickupInput->TryGetFeedback(Prompt.Text)) && !FocusHost->TryResolveFocusedPrompt(Prompt))
	{
		HidePrompt();
		return;
	}

	const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
	if (ViewportSize.X <= 0.0f
		|| ViewportSize.Y <= 0.0f
		|| !EnsurePromptWidget())
	{
		HidePrompt();
		return;
	}

	PromptWidget->SetPromptText(Prompt.Text);
	PromptWidget->SetPositionInViewport(
		ViewportSize * 0.5f + PromptViewportOffset,
		true);
	PromptWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
}

bool UFocusPromptPresenterComponent::EnsurePromptWidget()
{
	if (PromptWidget)
	{
		return true;
	}

	AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->GetLocalPlayer())
	{
		return false;
	}

	PromptWidget = CreateWidget<UInteractionPromptWidget>(
		PlayerController,
		UInteractionPromptWidget::StaticClass());
	if (!PromptWidget)
	{
		return false;
	}

	PromptWidget->SetAlignmentInViewport(FVector2D(0.5f, 0.0f));
	PromptWidget->SetVisibility(ESlateVisibility::Collapsed);
	PromptWidget->AddToViewport(20);
	return true;
}

void UFocusPromptPresenterComponent::HidePrompt()
{
	if (PromptWidget)
	{
		PromptWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
}
