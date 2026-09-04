#include "Items/AxeItemDefinition.h"

#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Items/AxeEquippedItemActor.h"
#include "Items/AxeSwingAbilityFeature.h"

UAxeItemDefinition::UAxeItemDefinition()
{
	UItemDisplayFeature* Display =
		CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT("ElementSandbox", "AxeItem", "斧头");
	Display->Tint = FLinearColor(0.58f, 0.66f, 0.72f, 1.0f);

	UItemStackFeature* Stack =
		CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 1;

	UEquippableItemFeature* Equippable =
		CreateDefaultSubobject<UEquippableItemFeature>(TEXT("Equippable"));
	Equippable->EquippedActorClass = AAxeEquippedItemActor::StaticClass();
	Equippable->AttachmentSocket = TEXT("hand_r");
	Equippable->AttachmentTransform = FTransform(
		// 斧柄从掌心向下延伸，与现有挥击动画的握持方向保持一致。
		FRotator(90.0, 0.0, 0.0),
		FVector(0.0, 0.0, 2.0));

	UAxeSwingAbilityFeature* Swing =
		CreateDefaultSubobject<UAxeSwingAbilityFeature>(TEXT("SwingAbility"));

	FeatureTemplates = {Display, Stack, Equippable, Swing};
}
