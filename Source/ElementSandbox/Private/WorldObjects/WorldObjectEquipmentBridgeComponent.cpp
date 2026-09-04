#include "WorldObjects/WorldObjectEquipmentBridgeComponent.h"

#include "Definition/WorldObjectDefinition.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Equipment/EquipmentComponent.h"
#include "Inventory/InventoryComponent.h"
#include "Item/ItemInstance.h"
#include "Items/StickEquippedItemActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"

UWorldObjectEquipmentBridgeComponent::UWorldObjectEquipmentBridgeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UWorldObjectEquipmentBridgeComponent::BeginPlay()
{
	Super::BeginPlay();
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor) || !OwnerActor->HasAuthority())
	{
		return;
	}

	if (UEquipmentComponent* Equipment =
		OwnerActor->FindComponentByClass<UEquipmentComponent>())
	{
		Equipment->OnEquippedItemChanged().AddUObject(
			this,
			&UWorldObjectEquipmentBridgeComponent::HandleEquippedItemChanged);
		if (UItemInstance* CurrentItem = Equipment->GetCurrentEquippedItem())
		{
			CreateAttachedProjection(*CurrentItem);
		}
	}
}

void UWorldObjectEquipmentBridgeComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (AActor* OwnerActor = GetOwner())
	{
		if (UEquipmentComponent* Equipment =
			OwnerActor->FindComponentByClass<UEquipmentComponent>())
		{
			Equipment->OnEquippedItemChanged().RemoveAll(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UWorldObjectEquipmentBridgeComponent::HandleEquippedItemChanged(
	UItemInstance* PreviousItem,
	UItemInstance* NewItem)
{
	if (IsValid(NewItem))
	{
		CreateAttachedProjection(*NewItem);
	}
}

void UWorldObjectEquipmentBridgeComponent::CreateAttachedProjection(
	UItemInstance& ItemInstance)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	UEquipmentComponent* Equipment = IsValid(OwnerActor)
		? OwnerActor->FindComponentByClass<UEquipmentComponent>()
		: nullptr;
	UWorldObjectItemCatalogSubsystem* Catalog = World
		? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>()
		: nullptr;
	UWorldObjectWorldSubsystem* WorldObjects = World
		? World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	AStickEquippedItemActor* StickActor = Equipment
		? Cast<AStickEquippedItemActor>(Equipment->GetEquippedActor())
		: nullptr;
	UWorldObjectDefinition* Definition = Catalog
		? Catalog->FindWorldObjectDefinition(ItemInstance.GetDefinition().GetObject())
		: nullptr;
	UWorldObjectProxyComponent* Proxy = IsValid(StickActor)
		? StickActor->GetWorldObjectProxyComponent()
		: nullptr;
	if (!IsValid(OwnerActor)
		|| !OwnerActor->HasAuthority()
		|| !WorldObjects
		|| !Definition
		|| !Proxy)
	{
		return;
	}

	if (WorldObjects->IsEntityAlive(Proxy->GetLocalEntity()))
	{
		return;
	}

	StickActor->ApplyHeldGripOffset();
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Definition;
	Desc.WorldTransform = StickActor->GetActorTransform();
	Desc.MotionState = EWorldObjectMotionState::Attached;
	Desc.Proxy = Proxy;
	WorldObjects->CreateEntity(Desc);
}

bool UWorldObjectEquipmentBridgeComponent::ThrowSelectedStick(
	UInventoryComponent& Inventory,
	const FVector& ForwardDirection,
	const double ForwardSpeed,
	const double UpwardSpeed)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	UEquipmentComponent* Equipment = IsValid(OwnerActor)
		? OwnerActor->FindComponentByClass<UEquipmentComponent>()
		: nullptr;
	UWorldObjectItemCatalogSubsystem* Catalog = World
		? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>()
		: nullptr;
	UWorldObjectWorldSubsystem* WorldObjects = World
		? World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	UItemInstance* CurrentItem = Equipment ? Equipment->GetCurrentEquippedItem() : nullptr;
	AStickEquippedItemActor* StickActor = Equipment
		? Cast<AStickEquippedItemActor>(Equipment->GetEquippedActor())
		: nullptr;
	UWorldObjectProxyComponent* Proxy = IsValid(StickActor)
		? StickActor->GetWorldObjectProxyComponent()
		: nullptr;
	const FWorldObjectEntityHandle Entity = Proxy
		? Proxy->GetLocalEntity()
		: FWorldObjectEntityHandle();
	const FWorldObjectMotionFragment* Motion = WorldObjects
		? WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity)
		: nullptr;
	if (!IsValid(OwnerActor)
		|| !OwnerActor->HasAuthority()
		|| !Equipment
		|| !Catalog
		|| !WorldObjects
		|| !IsValid(CurrentItem)
		|| !IsValid(StickActor)
		|| !Proxy
		|| !WorldObjects->IsEntityAlive(Entity)
		|| !Motion
		|| Motion->State != EWorldObjectMotionState::Attached
		|| Catalog->FindWorldObjectDefinition(CurrentItem->GetDefinition().GetObject()) == nullptr
		|| !StickActor->CanBeginServerThrow()
		|| ForwardDirection.IsNearlyZero()
		|| !FMath::IsFinite(ForwardSpeed)
		|| !FMath::IsFinite(UpwardSpeed)
		|| ForwardSpeed < 0.0)
	{
		return false;
	}

	UItemInstance* ExtractedItem = nullptr;
	AEquippedItemActor* ReleasedActor = nullptr;
	if (!Inventory.ExtractSelectedEquippedItem(ExtractedItem, ReleasedActor)
		|| ExtractedItem != CurrentItem
		|| ReleasedActor != StickActor)
	{
		return false;
	}

	if (!WorldObjects->SetMotionState(Entity, EWorldObjectMotionState::Physics)
		|| !StickActor->BeginServerThrow(
			ForwardDirection.GetSafeNormal() * ForwardSpeed
				+ FVector::UpVector * UpwardSpeed))
	{
		// 所有可能失败的条件已在提取前验证；这里仅是保护损坏状态的兜底。
		WorldObjects->DestroyEntity(Entity);
		Inventory.AddItem(CurrentItem->GetDefinition(), 1, EInventoryContainer::Quickbar);
		return false;
	}

	return true;
}
