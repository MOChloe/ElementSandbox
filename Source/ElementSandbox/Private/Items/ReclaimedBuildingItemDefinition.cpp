#include "Items/ReclaimedBuildingItemDefinition.h"

#include "Building/BuildingItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"

UReclaimedBuildingItemDefinition::UReclaimedBuildingItemDefinition()
{
	UItemDisplayFeature* Display =
		CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT(
		"ElementSandbox",
		"ReclaimedBuildingItem",
		"回收建筑构件");
	Display->Tint = FLinearColor(0.72f, 0.58f, 0.40f, 1.0f);

	UItemStackFeature* Stack =
		CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 1;

	UBuildingItemFeature* Building =
		CreateDefaultSubobject<UBuildingItemFeature>(TEXT("Building"));

	FeatureTemplates = {Display, Stack, Building};
}
