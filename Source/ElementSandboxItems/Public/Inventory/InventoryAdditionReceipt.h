#pragma once

#include "CoreMinimal.h"
#include "Inventory/InventoryTypes.h"
#include "UObject/StrongObjectPtr.h"

class UInventoryComponent;
class UItemInstance;

/**
 * 服务器同步调用栈内的一次背包增加事务。
 * 调用方必须立即 Commit 或 Rollback；它用于让世界对象移除与物品返还保持全有或全无。
 */
struct ELEMENTSANDBOXITEMS_API FInventoryAdditionReceipt final
{
	FInventoryAdditionReceipt() = default;
	FInventoryAdditionReceipt(const FInventoryAdditionReceipt&) = delete;
	FInventoryAdditionReceipt& operator=(const FInventoryAdditionReceipt&) = delete;
	FInventoryAdditionReceipt(FInventoryAdditionReceipt&&) = default;
	FInventoryAdditionReceipt& operator=(FInventoryAdditionReceipt&&) = default;

	bool IsActive() const { return bActive; }
	int32 GetAddedQuantity() const { return AddedQuantity; }

	/**
	 * 返回本事务唯一新建的 ItemInstance；若事务未激活、只扩充了既有堆叠或新建了多个实例则返回空。
	 * 供调用方在 Commit 前配置不可堆叠物品的运行期 Feature，不能把返回值跨帧保存。
	 */
	UItemInstance* GetSingleCreatedItem() const;

private:
	struct FChange final
	{
		FInventorySlotAddress Address;
		TStrongObjectPtr<UItemInstance> ItemInstance;
		int32 PreviousQuantity = 0;
		int32 ResultingQuantity = 0;
		bool bCreatedItem = false;
	};

	void Reset()
	{
		Inventory.Reset();
		Changes.Reset();
		AddedQuantity = 0;
		bActive = false;
	}

	TWeakObjectPtr<UInventoryComponent> Inventory;
	TArray<FChange> Changes;
	int32 AddedQuantity = 0;
	bool bActive = false;

	friend UInventoryComponent;
};
