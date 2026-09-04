#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Inventory/InventoryTypes.h"
#include "InventorySlotWidget.generated.h"

class UBorder;
class UDragDropOperation;
class UInventoryComponent;
class UItemInstance;
class USizeBox;
class UTextBlock;

/** 显示一个库存槽位，并把拖放意图提交给服务器权威 InventoryComponent。 */
UCLASS()
class UInventorySlotWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetSlotData(
		int32 ShortcutNumber,
		UInventoryComponent* InInventory,
		const FInventorySlotAddress& InAddress,
		UItemInstance* ItemInstance,
		bool bSelected);

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnDragDetected(
		const FGeometry& InGeometry,
		const FPointerEvent& InMouseEvent,
		UDragDropOperation*& OutOperation) override;
	virtual bool NativeOnDrop(
		const FGeometry& InGeometry,
		const FDragDropEvent& InDragDropEvent,
		UDragDropOperation* InOperation) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<USizeBox> SlotSize;

	/** 只负责细描边和选中强调，避免用整块高饱和颜色盖住内容。 */
	UPROPERTY(Transient)
	TObjectPtr<UBorder> FrameBorder;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> SlotBorder;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ShortcutText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ItemText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> QuantityText;

	UPROPERTY(Transient)
	TObjectPtr<UInventoryComponent> Inventory;

	UPROPERTY(Transient)
	TObjectPtr<UItemInstance> CurrentItem;

	FInventorySlotAddress Address;
};
