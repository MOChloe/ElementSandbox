#include "WorldObjects/WorldObjectPickupAuthorityService.h"

#include "GameFramework/Pawn.h"
#include "Inventory/InventoryAdditionReceipt.h"
#include "Inventory/InventoryComponent.h"
#include "Item/ItemDefinition.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectPickupResolver.h"

EWorldObjectPickupFailure FWorldObjectPickupAuthorityService::TryPickup(
	UWorldObjectWorldSubsystem& WorldObjects,
	const UWorldObjectItemCatalogSubsystem& Catalog,
	UInventoryComponent& Inventory,
	APawn& Pawn,
	const FWorldEntityId WorldEntityId,
	const double MaximumDistance)
{
	using namespace UE::ElementSandbox;
	if (!Pawn.HasAuthority() || !Pawn.GetWorld() || !FMath::IsFinite(MaximumDistance)
		|| MaximumDistance <= 0.0)
		return EWorldObjectPickupFailure::PlayerUnavailable;
	const FWorldObjectEntityHandle Entity = WorldObjects.FindEntity(WorldEntityId);
	FWorldObjectPickupResolution Pickup;
	if (!WorldEntityId.IsSet() || !TryResolveWorldObjectPickup(WorldObjects, Entity, Catalog, Pickup))
		return EWorldObjectPickupFailure::InvalidTarget;
	if (Pickup.ComputeSquaredInteractionDistance(Pawn.GetActorLocation()) > FMath::Square(MaximumDistance))
		return EWorldObjectPickupFailure::OutOfRange;
	if (!HasClearWorldObjectPickupPath(*Pawn.GetWorld(), Pawn, Pickup, Pawn.GetActorLocation()))
		return EWorldObjectPickupFailure::Obstructed;
	if (!Inventory.CanAddItem(Pickup.ItemDefinition, Pickup.Quantity))
		return EWorldObjectPickupFailure::InventoryFull;

	FInventoryAdditionReceipt Receipt;
	int32 AddedQuantity = 0;
	if (!Inventory.BeginAddItem(Pickup.ItemDefinition, Pickup.Quantity,
		EInventoryContainer::Backpack, Receipt, AddedQuantity) || AddedQuantity != Pickup.Quantity)
	{
		if (Receipt.IsActive())
			ensureMsgf(Inventory.RollbackItemAddition(Receipt), TEXT("拾取预留失败必须完整撤销背包增加。"));
		return EWorldObjectPickupFailure::InventoryFull;
	}

	const bool bDestroyReportedSuccess = WorldObjects.DestroyEntity(Entity);
	if (WorldObjects.IsEntityAlive(Entity))
	{
		ensureMsgf(Inventory.RollbackItemAddition(Receipt), TEXT("物件拒绝 GameplayDestroy 后必须撤销拾取增加。"));
		return EWorldObjectPickupFailure::DestroyRejected;
	}
	// 派生投影的清理失败不能使已经终结的物件和背包奖励一起丢失。
	ensureMsgf(bDestroyReportedSuccess, TEXT("拾取宿主已终结，但派生清理报告失败。"));
	ensureMsgf(Inventory.CommitItemAddition(Receipt), TEXT("物件终结后拾取背包事务必须能提交。"));
	return EWorldObjectPickupFailure::None;
}
