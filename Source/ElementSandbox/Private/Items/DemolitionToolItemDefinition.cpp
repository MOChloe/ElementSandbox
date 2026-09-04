#include "Items/DemolitionToolItemDefinition.h"

#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Items/DemolitionToolEquippedItemActor.h"
#include "Items/DemolitionToolItemFeature.h"
#include "Items/DemolitionToolSwingAbilityFeature.h"
#include "Items/ReclaimedBuildingItemDefinition.h"
#include "Torch/TorchDefinition.h"
#include "UObject/ConstructorHelpers.h"

UDemolitionToolItemDefinition::UDemolitionToolItemDefinition()
{
	UItemDisplayFeature* Display =
		CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT("ElementSandbox", "DemolitionToolItem", "拆除锤");
	Display->Tint = FLinearColor(0.95f, 0.68f, 0.18f, 1.0f);

	UItemStackFeature* Stack =
		CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 1;

	UEquippableItemFeature* Equippable =
		CreateDefaultSubobject<UEquippableItemFeature>(TEXT("Equippable"));
	Equippable->EquippedActorClass = ADemolitionToolEquippedItemActor::StaticClass();
	Equippable->AttachmentSocket = TEXT("hand_r");
	Equippable->AttachmentTransform = FTransform(
		// 锤柄从掌心向下延伸，避免锤头沿前臂反向落到肩旁。
		FRotator(90.0, 0.0, 0.0),
		FVector(0.0, 0.0, 2.0));

	UDemolitionToolSwingAbilityFeature* Swing =
		CreateDefaultSubobject<UDemolitionToolSwingAbilityFeature>(TEXT("SwingAbility"));

	UDemolitionToolItemFeature* Dismantle =
		CreateDefaultSubobject<UDemolitionToolItemFeature>(TEXT("Dismantle"));
	static ConstructorHelpers::FObjectFinder<UItemDefinition> WallItem(
		TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"));
	static ConstructorHelpers::FObjectFinder<UItemDefinition> FloorItem(
		TEXT("/Game/Items/DA_WoodFloor.DA_WoodFloor"));
	static ConstructorHelpers::FObjectFinder<UItemDefinition> PillarItem(
		TEXT("/Game/Items/DA_WoodPillar.DA_WoodPillar"));
	const auto AddReward = [Dismantle](
		const FName BuildingDefinitionId,
		UItemDefinition* ItemDefinition,
		const bool bPreservePlacementShape = false)
	{
		if (BuildingDefinitionId.IsNone() || !IsValid(ItemDefinition))
		{
			return;
		}
		FBuildingDismantleReward& Reward = Dismantle->Rewards.AddDefaulted_GetRef();
		Reward.BuildingDefinitionId = BuildingDefinitionId;
		Reward.ItemDefinition = ItemDefinition;
		Reward.bPreservePlacementShape = bPreservePlacementShape;
	};
	AddReward(TEXT("WoodWall"), WallItem.Object);
	AddReward(TEXT("WoodFloor"), FloorItem.Object);
	AddReward(TEXT("WoodPillar"), PillarItem.Object);

	// 离线 AI 只负责预置玩家本可摆放的城镇。回收物必须保存每实例形态，不能把
	// 任意尺寸/倾角的配方构件粗暴降级成快捷栏里的单位木墙。
	UReclaimedBuildingItemDefinition* ReclaimedBuilding =
		GetMutableDefault<UReclaimedBuildingItemDefinition>();
	for (const ECityBuildingPieceKind Kind : GetDefaultCityPrimitivePieceKinds())
	{
		for (const FName SurfaceProfileId : GetDefaultCityPieceSurfaceProfileIds())
		{
			AddReward(
				GetCityBuildingPieceDefinitionId(Kind, SurfaceProfileId),
				ReclaimedBuilding,
				true);
		}
	}
	AddReward(TEXT("Door"), ReclaimedBuilding, true);
	AddReward(TEXT("Settlement.Door"), ReclaimedBuilding, true);
	AddReward(GetMountedTorchBuildingDefinitionId(), ReclaimedBuilding, true);

	FeatureTemplates = {Display, Stack, Equippable, Swing, Dismantle};
}
