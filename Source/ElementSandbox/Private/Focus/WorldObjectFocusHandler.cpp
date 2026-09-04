#include "Focus/WorldObjectFocusHandler.h"

#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Engine/World.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Focus/FocusQueryTypes.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Game/ElementSandboxPlayerController.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupResolver.h"
#include "WorldObjects/WorldObjectPickupComponent.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/ItemDefinition.h"

bool UWorldObjectFocusHandler::IsSameTargetImpl(
	const FFocusQueryHit& Left,
	const FFocusQueryHit& Right) const
{
	const FWorldObjectFocusTarget* LeftTarget =
		Left.Target.GetPtr<FWorldObjectFocusTarget>();
	const FWorldObjectFocusTarget* RightTarget =
		Right.Target.GetPtr<FWorldObjectFocusTarget>();
	return LeftTarget
		&& RightTarget
		&& LeftTarget->WorldEntityId == RightTarget->WorldEntityId;
}

bool UWorldObjectFocusHandler::TryResolvePromptImpl(
	const FFocusQueryHit& Hit,
	FFocusInteractionPrompt& OutPrompt) const
{
	const FWorldObjectFocusTarget* Target =
		Hit.Target.GetPtr<FWorldObjectFocusTarget>();
	const AElementSandboxPlayerController* PlayerController =
		GetTypedOuter<AElementSandboxPlayerController>();
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	const UWorldObjectWorldSubsystem* WorldObjects = World
		? World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	const UWorldObjectItemCatalogSubsystem* Catalog = World
		? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>()
		: nullptr;
	const FWorldObjectEntityHandle Entity = WorldObjects && Target
		? WorldObjects->FindEntity(Target->WorldEntityId)
		: FWorldObjectEntityHandle();
	if (!Target
		|| !Target->IsValid()
		|| !PlayerController
		|| !PlayerController->IsLocalController()
		|| !WorldObjects
		|| !Catalog
		|| !WorldObjects->IsEntityAlive(Entity))
	{
		return false;
	}

	const auto* PickupInput = PlayerController->FindComponentByClass<UWorldObjectPickupComponent>();
	UE::ElementSandbox::FWorldObjectPickupResolution Pickup;
	if ((PickupInput && PickupInput->IsTargetUnavailable(Target->WorldEntityId))
		|| !UE::ElementSandbox::TryResolveWorldObjectPickup(
			*WorldObjects,
			Entity,
			*Catalog,
			Pickup))
	{
		return false;
	}

	const auto* Display = Pickup.ItemDefinition->FindFeatureTemplate<UItemDisplayFeature>();
	const FText Name = Display && !Display->DisplayName.IsEmpty() ? Display->DisplayName
		: NSLOCTEXT("ElementSandbox", "PickupUnnamedItem", "物品");
	OutPrompt.Text = FText::Format(NSLOCTEXT("ElementSandbox", "PickupWorldObjectPrompt",
		"E 拾取 {0} · 按住连续收集"), Name);
	return true;
}

bool UWorldObjectFocusHandler::HandleInteractImpl(const FFocusQueryHit& Hit)
{
	const FWorldObjectFocusTarget* Target =
		Hit.Target.GetPtr<FWorldObjectFocusTarget>();
	AElementSandboxPlayerController* PlayerController =
		GetTypedOuter<AElementSandboxPlayerController>();
	if (!Target || !Target->IsValid() || !PlayerController
		|| !PlayerController->IsLocalController())
	{
		return false;
	}

	return PlayerController->RequestPickupWorldObject(Target->WorldEntityId);
}
