#pragma once

#include "CoreMinimal.h"
#include "Inventory/InventoryTypes.h"
#include "UObject/StrongObjectPtr.h"

class UInventoryComponent;
class UItemInstance;

/**
 * 服务器短事务内持有一次数量扣除的完整回滚信息。
 * 调用方必须在同一个同步调用栈内 Commit 或 Rollback，不得跨帧保存。
 */
struct ELEMENTSANDBOXITEMS_API FInventoryConsumptionReceipt final
{
	FInventoryConsumptionReceipt() = default;
	FInventoryConsumptionReceipt(const FInventoryConsumptionReceipt&) = delete;
	FInventoryConsumptionReceipt& operator=(const FInventoryConsumptionReceipt&) = delete;
	FInventoryConsumptionReceipt(FInventoryConsumptionReceipt&&) = default;
	FInventoryConsumptionReceipt& operator=(FInventoryConsumptionReceipt&&) = default;

	bool IsActive() const { return bActive; }

private:
	void Reset()
	{
		Inventory.Reset();
		ItemInstance.Reset();
		Address = {};
		PreviousQuantity = 0;
		ConsumedQuantity = 0;
		PreviousSelectedQuickbarIndex = INDEX_NONE;
		bRemovedWholeInstance = false;
		bActive = false;
	}

	TWeakObjectPtr<UInventoryComponent> Inventory;
	TStrongObjectPtr<UItemInstance> ItemInstance;
	FInventorySlotAddress Address;
	int32 PreviousQuantity = 0;
	int32 ConsumedQuantity = 0;
	int32 PreviousSelectedQuickbarIndex = INDEX_NONE;
	bool bRemovedWholeInstance = false;
	bool bActive = false;

	friend UInventoryComponent;
};
