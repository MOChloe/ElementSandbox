#include "Focus/BuildingFocusHandler.h"

#include "BuildingWorldSubsystem.h"
#include "Door/DoorInteractionResolver.h"
#include "Definition/BuildingDefinition.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Focus/BuildingFocusTarget.h"
#include "Focus/FocusQueryTypes.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/ItemDefinition.h"

bool UBuildingFocusHandler::IsSameTargetImpl(
	const FFocusQueryHit& Left,
	const FFocusQueryHit& Right) const
{
	const FBuildingFocusTarget* LeftTarget = Left.Target.GetPtr<FBuildingFocusTarget>();
	const FBuildingFocusTarget* RightTarget = Right.Target.GetPtr<FBuildingFocusTarget>();
	return LeftTarget
		&& RightTarget
		&& LeftTarget->Entity == RightTarget->Entity;
}

bool UBuildingFocusHandler::TryResolvePromptImpl(
	const FFocusQueryHit& Hit,
	FFocusInteractionPrompt& OutPrompt) const
{
	const FBuildingFocusTarget* Target = Hit.Target.GetPtr<FBuildingFocusTarget>();
	const AElementSandboxPlayerController* PlayerController =
		GetTypedOuter<AElementSandboxPlayerController>();
	if (!Target
		|| !PlayerController
		|| !PlayerController->IsLocalController()
		|| !PlayerController->GetWorld())
	{
		return false;
	}

	const UBuildingWorldSubsystem* BuildingSubsystem =
		PlayerController->GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
	if (!BuildingSubsystem)
	{
		return false;
	}

	const FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
	if (PlayerController->IsDemolitionToolSelected())
	{
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(Target->Entity);
		const UBuildingDefinition* Definition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		if (!Definition)
		{
			return false;
		}
		UItemDefinition* RewardItem = nullptr;
		int32 RewardQuantity = 0;
		if (PlayerController->TryResolveSelectedDismantleReward(
				Definition->DefinitionId,
				RewardItem,
				RewardQuantity))
		{
			FText RewardName = FText::FromName(Definition->DefinitionId);
			if (const UItemDisplayFeature* Display = RewardItem
				? RewardItem->FindFeatureTemplate<UItemDisplayFeature>()
				: nullptr;
				Display && !Display->DisplayName.IsEmpty())
			{
				RewardName = Display->DisplayName;
			}
			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("Quantity"), FText::AsNumber(RewardQuantity));
			Arguments.Add(TEXT("Item"), RewardName);
			OutPrompt.Text = FText::Format(
				NSLOCTEXT(
					"ElementSandbox",
					"DismantleBuildingPrompt",
					"左键拆除（返还 {Quantity} × {Item}）"),
				Arguments);
		}
		else
		{
			OutPrompt.Text = NSLOCTEXT(
				"ElementSandbox",
				"UnsupportedDismantleBuildingPrompt",
				"不可拆除：没有对应返还物品");
		}
		return true;
	}

	EBuildDoorInteractionIntent Intent = EBuildDoorInteractionIntent::None;
	if (!TryResolveBuildDoorInteraction(Registry, Target->Entity, Intent))
	{
		return false;
	}

	const bool bOpen = Intent == EBuildDoorInteractionIntent::Close;
	OutPrompt.Text = bOpen
		? NSLOCTEXT("ElementSandbox", "CloseDoorPrompt", "按 E 关门")
		: NSLOCTEXT("ElementSandbox", "OpenDoorPrompt", "按 E 开门");
	return true;
}

bool UBuildingFocusHandler::HandleInteractImpl(const FFocusQueryHit& Hit)
{
	const FBuildingFocusTarget* Target = Hit.Target.GetPtr<FBuildingFocusTarget>();
	AElementSandboxPlayerController* PlayerController =
		GetTypedOuter<AElementSandboxPlayerController>();
	if (!Target
		|| !PlayerController
		|| !PlayerController->IsLocalController()
		|| !PlayerController->GetWorld())
	{
		return false;
	}

	UBuildingWorldSubsystem* BuildingSubsystem =
		PlayerController->GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
	if (!BuildingSubsystem)
	{
		return false;
	}

	const FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
	EBuildDoorInteractionIntent Intent = EBuildDoorInteractionIntent::None;
	if (!TryResolveBuildDoorInteraction(Registry, Target->Entity, Intent))
	{
		return false;
	}

	const FWorldEntityId WorldEntityId = BuildingSubsystem->GetWorldEntityId(Target->Entity);
	return PlayerController->RequestDoorInteraction(WorldEntityId);
}

bool UBuildingFocusHandler::HandlePrimaryUseImpl(const FFocusQueryHit& Hit)
{
	const FBuildingFocusTarget* Target = Hit.Target.GetPtr<FBuildingFocusTarget>();
	AElementSandboxPlayerController* PlayerController =
		GetTypedOuter<AElementSandboxPlayerController>();
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!Target || !PlayerController || !PlayerController->IsLocalController()
		|| !BuildingSubsystem || !BuildingSubsystem->IsEntityAlive(Target->Entity))
	{
		return false;
	}

	const FBuildDefinitionFragment* DefinitionFragment = BuildingSubsystem->GetRegistry()
		.FindFragment<FBuildDefinitionFragment>(Target->Entity);
	const UBuildingDefinition* Definition = DefinitionFragment
		? DefinitionFragment->Definition.Get()
		: nullptr;
	UItemDefinition* RewardItem = nullptr;
	int32 RewardQuantity = 0;
	if (!Definition || !PlayerController->IsDemolitionToolSelected())
	{
		return false;
	}
	if (!PlayerController->TryResolveSelectedDismantleReward(
			Definition->DefinitionId,
			RewardItem,
			RewardQuantity))
	{
		// 无返还映射的 Building 仍消费本次拆除锤输入；提示已经解释拒绝原因。
		return true;
	}

	return PlayerController->RequestBuildingDismantle(
		BuildingSubsystem->GetWorldEntityId(Target->Entity));
}
