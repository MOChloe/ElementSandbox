#include "Inventory/InventoryQuickbarWidget.h"

#include "Inventory/InventoryComponent.h"
#include "Inventory/InventorySlotWidget.h"
#include "Item/ItemInstance.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Style/ElementSandboxUIStyle.h"

void UInventoryQuickbarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UBorder* QuickbarFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("QuickbarFrame"));
	QuickbarFrame->SetPadding(FMargin(5.0f));
	QuickbarFrame->SetBrushColor(ElementSandbox::UIStyle::PanelBackground);
	WidgetTree->RootWidget = QuickbarFrame;

	SlotBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("SlotBox"));
	QuickbarFrame->SetContent(SlotBox);

	SlotWidgets.Reserve(UInventoryComponent::QuickbarSlotCount);
	for (int32 Index = 0; Index < UInventoryComponent::QuickbarSlotCount; ++Index)
	{
		UInventorySlotWidget* SlotWidget = WidgetTree->ConstructWidget<UInventorySlotWidget>(
			UInventorySlotWidget::StaticClass());
		SlotWidgets.Add(SlotWidget);
		UHorizontalBoxSlot* BoxSlot = SlotBox->AddChildToHorizontalBox(SlotWidget);
		BoxSlot->SetPadding(FMargin(2.0f, 0.0f));
	}
	Refresh();
}

void UInventoryQuickbarWidget::SetInventory(UInventoryComponent* InInventory)
{
	if (Inventory == InInventory)
	{
		Refresh();
		return;
	}

	UnbindInventory();
	Inventory = InInventory;
	if (Inventory)
	{
		InventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
			this, &UInventoryQuickbarWidget::Refresh);
	}
	Refresh();
}

void UInventoryQuickbarWidget::Refresh()
{
	for (int32 Index = 0; Index < SlotWidgets.Num(); ++Index)
	{
		const FInventorySlotAddress Address(EInventoryContainer::Quickbar, Index);
		UItemInstance* Item = Inventory ? Inventory->GetItem(Address) : nullptr;
		const bool bSelected = Inventory && Inventory->GetSelectedQuickbarIndex() == Index;
		SlotWidgets[Index]->SetSlotData(
			Index, Inventory, Address, Item, bSelected);
	}
}

void UInventoryQuickbarWidget::UnbindInventory()
{
	if (Inventory && InventoryChangedHandle.IsValid())
	{
		Inventory->OnInventoryChanged().Remove(InventoryChangedHandle);
	}
	InventoryChangedHandle.Reset();
}

void UInventoryQuickbarWidget::NativeDestruct()
{
	UnbindInventory();
	Super::NativeDestruct();
}
