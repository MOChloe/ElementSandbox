#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InventoryBackpackWidget.generated.h"

class UInventoryComponent;
class UInventorySlotWidget;
class UUniformGridPanel;

/** 左侧 6x5 背包面板；槽位可与快捷栏双向拖放。 */
UCLASS()
class UInventoryBackpackWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetInventory(UInventoryComponent* InInventory);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UUniformGridPanel> SlotGrid;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInventorySlotWidget>> SlotWidgets;

	UPROPERTY(Transient)
	TObjectPtr<UInventoryComponent> Inventory;

	FDelegateHandle InventoryChangedHandle;

	void Refresh();
	void UnbindInventory();
};
