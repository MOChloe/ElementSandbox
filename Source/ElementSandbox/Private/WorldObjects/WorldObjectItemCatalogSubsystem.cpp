#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"

#include "Item/ItemDefinition.h"
#include "Items/WoodBlockItemDefinition.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/CharcoalWorldObjectDefinition.h"
#include "WorldObjects/StickWorldObjectDefinition.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

void UWorldObjectItemCatalogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	StickItemDefinition = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Stick.DA_Stick"));
	StickWorldObjectDefinition = GetMutableDefault<UStickWorldObjectDefinition>();
	WoodBlockItemDefinition = GetMutableDefault<UWoodBlockItemDefinition>();
	WoodBlockWorldObjectDefinition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
	CharcoalItemDefinition = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Charcoal.DA_Charcoal"));
	CharcoalWorldObjectDefinition = GetMutableDefault<UCharcoalWorldObjectDefinition>();
	if (UWorldObjectWorldSubsystem* WorldObjects =
		GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		bDefinitionsRegistered =
			WorldObjects->RegisterDefinition(*StickWorldObjectDefinition)
			&& WorldObjects->RegisterDefinition(*WoodBlockWorldObjectDefinition)
			&& WorldObjects->RegisterDefinition(*CharcoalWorldObjectDefinition);
	}
}

UWorldObjectDefinition* UWorldObjectItemCatalogSubsystem::FindWorldObjectDefinition(
	const UObject* ItemDefinition) const
{
	if (!IsReady())
	{
		return nullptr;
	}
	if (ItemDefinition == StickItemDefinition)
	{
		return StickWorldObjectDefinition.Get();
	}
	if (ItemDefinition == CharcoalItemDefinition)
	{
		return CharcoalWorldObjectDefinition.Get();
	}
	return ItemDefinition == WoodBlockItemDefinition
		? WoodBlockWorldObjectDefinition.Get()
		: nullptr;
}

UItemDefinition* UWorldObjectItemCatalogSubsystem::FindItemDefinition(
	const UWorldObjectDefinition* WorldObjectDefinition) const
{
	if (!IsReady())
	{
		return nullptr;
	}
	if (WorldObjectDefinition == StickWorldObjectDefinition)
	{
		return StickItemDefinition.Get();
	}
	if (WorldObjectDefinition == CharcoalWorldObjectDefinition)
	{
		return CharcoalItemDefinition.Get();
	}
	return WorldObjectDefinition == WoodBlockWorldObjectDefinition
		? WoodBlockItemDefinition.Get()
		: nullptr;
}

bool UWorldObjectItemCatalogSubsystem::IsReady() const
{
	return bDefinitionsRegistered
		&& IsValid(StickItemDefinition)
		&& IsValid(StickWorldObjectDefinition)
		&& IsValid(WoodBlockItemDefinition)
		&& IsValid(WoodBlockWorldObjectDefinition)
		&& IsValid(CharcoalItemDefinition)
		&& IsValid(CharcoalWorldObjectDefinition);
}

bool UWorldObjectItemCatalogSubsystem::DoesSupportWorldType(
	const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
