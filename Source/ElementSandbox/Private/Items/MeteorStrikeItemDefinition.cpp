#include "Items/MeteorStrikeItemDefinition.h"

#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Items/MeteorStrikeAbilityFeature.h"
#include "Items/MeteorStrikeEquippedItemActor.h"

UMeteorStrikeItemDefinition::UMeteorStrikeItemDefinition()
{
	UItemDisplayFeature* Display = CreateDefaultSubobject<UItemDisplayFeature>(TEXT("Display"));
	Display->DisplayName = NSLOCTEXT("ElementSandbox", "MeteorStrikeItem", "陨石核心");
	Display->Tint = FLinearColor(0.45f, 0.04f, 0.01f, 1.0f);
	UItemStackFeature* Stack = CreateDefaultSubobject<UItemStackFeature>(TEXT("Stack"));
	Stack->MaxStackSize = 1;
	UEquippableItemFeature* Equippable = CreateDefaultSubobject<UEquippableItemFeature>(TEXT("Equippable"));
	Equippable->EquippedActorClass = AMeteorStrikeEquippedItemActor::StaticClass();
	Equippable->AttachmentSocket = TEXT("hand_r");
	UMeteorStrikeAbilityFeature* Ability = CreateDefaultSubobject<UMeteorStrikeAbilityFeature>(TEXT("Ability"));
	FeatureTemplates = {Display, Stack, Equippable, Ability};
}
