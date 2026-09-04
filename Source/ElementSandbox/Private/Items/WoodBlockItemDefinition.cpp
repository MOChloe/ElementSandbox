#include "Items/WoodBlockItemDefinition.h"

#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"

UWoodBlockItemDefinition::UWoodBlockItemDefinition()
{
	UItemDisplayFeature* Display =
		CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT("ElementSandbox", "WoodBlockItem", "木块");
	Display->Tint = FLinearColor(0.48f, 0.24f, 0.08f, 1.0f);

	UItemStackFeature* Stack =
		CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 99;

	FeatureTemplates = {Display, Stack};
}

