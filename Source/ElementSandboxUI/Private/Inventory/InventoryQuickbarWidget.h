#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InventoryQuickbarWidget.generated.h"

class UHorizontalBox;
class UInventoryComponent;
class UInventorySlotWidget;

/** 显示 10 个快捷栏槽位，并通过 InventoryComponent 事件刷新。 */
UCLASS()
class UInventoryQuickbarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetInventory(UInventoryComponent* InInventory);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UHorizontalBox> SlotBox;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInventorySlotWidget>> SlotWidgets;

	UPROPERTY(Transient)
	TObjectPtr<UInventoryComponent> Inventory;

	FDelegateHandle InventoryChangedHandle;

	void Refresh();
	void UnbindInventory();
};
