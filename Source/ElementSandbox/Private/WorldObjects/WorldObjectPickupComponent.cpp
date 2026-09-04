#include "WorldObjects/WorldObjectPickupComponent.h"

#include "Engine/World.h"
#include "Focus/FocusHostComponent.h"
#include "Game/ElementSandboxPlayerController.h"
#include "GameFramework/Pawn.h"
#include "WorldObjectWorldSubsystem.h"

UWorldObjectPickupComponent::UWorldObjectPickupComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UWorldObjectPickupComponent::BeginPlay()
{
	Super::BeginPlay();
	const auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (Controller && Controller->IsLocalController())
		if (auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>())
			AddTickPrerequisiteComponent(Focus);
}

void UWorldObjectPickupComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndInteract();
	PendingTarget = {};
	AwaitingTombstones.Reset();
	Super::EndPlay(EndPlayReason);
}

double UWorldObjectPickupComponent::GetNow() const
{
	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
}

bool UWorldObjectPickupComponent::BeginInteract()
{
	auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	auto* Focus = Controller ? Controller->FindComponentByClass<UFocusHostComponent>() : nullptr;
	if (!Controller || !Controller->CanUseFocusInteraction() || !Controller->GetPawn() || !Focus)
		return false;
	if (bCollecting) return false;
	const FFocusQueryHit* Hit = Focus->GetFocusedHit();
	RejectedTarget = {};
	Feedback = FText::GetEmpty();
	if (!Hit || Hit->bRepeatableInteract)
	{
		bCollecting = true;
		CollectingPawn = Controller->GetPawn();
		NextRepeatTime = GetNow() + 0.35;
		Focus->SetRepeatableInteractOnly(true);
		SetComponentTickEnabled(true);
	}
	// 门只在此处接收按下事件，持续收集永远不重复门操作。
	return Focus->HandleInteract();
}

void UWorldObjectPickupComponent::EndInteract()
{
	bCollecting = false;
	CollectingPawn.Reset();
	if (const auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner()))
		if (auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>())
			Focus->SetRepeatableInteractOnly(false);
}

bool UWorldObjectPickupComponent::TryBeginRequest(const FWorldEntityId WorldEntityId)
{
	PruneCompletedTargets();
	if (!WorldEntityId.IsSet() || PendingTarget.IsSet()
		|| AwaitingTombstones.Contains(WorldEntityId) || AwaitingTombstones.Num() >= 32)
		return false;
	PendingTarget = WorldEntityId;
	Feedback = FText::GetEmpty();
	SetComponentTickEnabled(true);
	return true;
}

void UWorldObjectPickupComponent::CompleteRequest(
	const FWorldEntityId WorldEntityId, const EWorldObjectPickupFailure Failure)
{
	if (PendingTarget != WorldEntityId) return;
	PendingTarget = {};
	if (Failure == EWorldObjectPickupFailure::None)
	{
		AwaitingTombstones.Add(WorldEntityId);
		PruneCompletedTargets();
		return;
	}
	RejectedTarget = WorldEntityId;
	RejectedUntil = GetNow() + 0.6;
	FeedbackUntil = GetNow() + 1.5;
	switch (Failure)
	{
	case EWorldObjectPickupFailure::InventoryFull:
		Feedback = NSLOCTEXT("ElementSandbox", "PickupInventoryFull", "背包已满");
		EndInteract();
		break;
	case EWorldObjectPickupFailure::OutOfRange:
		Feedback = NSLOCTEXT("ElementSandbox", "PickupOutOfRange", "物件已超出拾取范围");
		break;
	case EWorldObjectPickupFailure::Obstructed:
		Feedback = NSLOCTEXT("ElementSandbox", "PickupObstructed", "物件被挡住了");
		break;
	case EWorldObjectPickupFailure::DestroyRejected:
		Feedback = NSLOCTEXT("ElementSandbox", "PickupUnavailable", "暂时无法拾取");
		EndInteract();
		break;
	case EWorldObjectPickupFailure::PlayerUnavailable:
		EndInteract();
		break;
	default:
		break;
	}
}

bool UWorldObjectPickupComponent::IsTargetUnavailable(const FWorldEntityId WorldEntityId) const
{
	return WorldEntityId == PendingTarget || AwaitingTombstones.Contains(WorldEntityId)
		|| (WorldEntityId == RejectedTarget && GetNow() < RejectedUntil);
}

bool UWorldObjectPickupComponent::TryGetFeedback(FText& OutText) const
{
	if (Feedback.IsEmpty() || GetNow() >= FeedbackUntil) return false;
	OutText = Feedback;
	return true;
}

void UWorldObjectPickupComponent::PruneCompletedTargets()
{
	const auto* Objects = GetWorld() ? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>() : nullptr;
	for (auto It = AwaitingTombstones.CreateIterator(); It; ++It)
		if (!Objects || !Objects->FindEntity(*It).IsSet()) It.RemoveCurrent();
}

void UWorldObjectPickupComponent::AdvanceCollection(const double NowSeconds)
{
	PruneCompletedTargets();
	if (!bCollecting) return;
	auto* Controller = Cast<AElementSandboxPlayerController>(GetOwner());
	if (!Controller || !Controller->CanUseFocusInteraction()
		|| !CollectingPawn.IsValid() || CollectingPawn.Get() != Controller->GetPawn())
	{
		EndInteract();
		return;
	}
	if (!FMath::IsFinite(NowSeconds) || NowSeconds < NextRepeatTime || PendingTarget.IsSet()) return;
	NextRepeatTime = NowSeconds + 0.18;
	if (auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>())
	{
		const FFocusQueryHit* Hit = Focus->GetFocusedHit();
		if (Hit && Hit->bRepeatableInteract) Focus->HandleInteract();
	}
}

void UWorldObjectPickupComponent::TickComponent(const float DeltaTime,
	const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AdvanceCollection(GetNow());
	if (!bCollecting && !PendingTarget.IsSet() && AwaitingTombstones.IsEmpty())
		SetComponentTickEnabled(false);
}
