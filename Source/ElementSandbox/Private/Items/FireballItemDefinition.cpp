#include "Items/FireballItemDefinition.h"

#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Items/FireballAbilityFeature.h"
#include "Items/FireballEquippedItemActor.h"

UFireballItemDefinition::UFireballItemDefinition()
{
	UItemDisplayFeature* Display =
		CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT("ElementSandbox", "FireballItem", "火焰球");
	Display->Tint = FLinearColor(1.0f, 0.18f, 0.02f, 1.0f);

	UItemStackFeature* Stack =
		CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 999;

	UEquippableItemFeature* Equippable =
		CreateDefaultSubobject<UEquippableItemFeature>(TEXT("Equippable"));
	Equippable->EquippedActorClass = AFireballEquippedItemActor::StaticClass();
	Equippable->AttachmentSocket = TEXT("hand_r");
	Equippable->AttachmentTransform = FTransform(
		FRotator::ZeroRotator,
		FVector(0.0, 0.0, 8.0));

	UFireballAbilityFeature* Ability =
		CreateDefaultSubobject<UFireballAbilityFeature>(TEXT("Ability"));
	FeatureTemplates = {Display, Stack, Equippable, Ability};
}
