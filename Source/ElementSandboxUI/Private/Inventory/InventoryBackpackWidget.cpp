#include "Inventory/InventoryBackpackWidget.h"

#include "Inventory/InventoryComponent.h"
#include "Inventory/InventorySlotWidget.h"
#include "Item/ItemInstance.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Style/ElementSandboxUIStyle.h"

void UInventoryBackpackWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UBorder* PanelBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PanelBorder"));
	PanelBorder->SetPadding(FMargin(14.0f));
	PanelBorder->SetBrushColor(ElementSandbox::UIStyle::OpaquePanelBackground);
	WidgetTree->RootWidget = PanelBorder;

	UVerticalBox* PanelContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PanelContent"));
	PanelBorder->SetContent(PanelContent);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	Title->SetText(NSLOCTEXT("ElementSandbox", "BackpackTitle", "背包"));
	FSlateFontInfo TitleFont = Title->GetFont();
	TitleFont.Size = ElementSandbox::UIStyle::TitleFontSize;
	Title->SetFont(TitleFont);
	Title->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::PrimaryText));
	PanelContent->AddChildToVerticalBox(Title);

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Hint"));
	Hint->SetText(NSLOCTEXT("ElementSandbox", "BackpackDragHint", "拖到快捷栏设置按键，拖回来收回"));
	Hint->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	UVerticalBoxSlot* HintSlot = PanelContent->AddChildToVerticalBox(Hint);
	HintSlot->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 10.0f));

	SlotGrid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("SlotGrid"));
	SlotGrid->SetSlotPadding(FMargin(3.0f));
	PanelContent->AddChildToVerticalBox(SlotGrid);

	SlotWidgets.Reserve(UInventoryComponent::BackpackSlotCount);
	for (int32 Index = 0; Index < UInventoryComponent::BackpackSlotCount; ++Index)
	{
		UInventorySlotWidget* SlotWidget = WidgetTree->ConstructWidget<UInventorySlotWidget>(
			UInventorySlotWidget::StaticClass());
		SlotWidgets.Add(SlotWidget);
		SlotGrid->AddChildToUniformGrid(SlotWidget, Index / 6, Index % 6);
	}
	Refresh();
}

void UInventoryBackpackWidget::SetInventory(UInventoryComponent* InInventory)
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
			this, &UInventoryBackpackWidget::Refresh);
	}
	Refresh();
}

void UInventoryBackpackWidget::Refresh()
{
	for (int32 Index = 0; Index < SlotWidgets.Num(); ++Index)
	{
		const FInventorySlotAddress Address(EInventoryContainer::Backpack, Index);
		UItemInstance* Item = Inventory ? Inventory->GetItem(Address) : nullptr;
		SlotWidgets[Index]->SetSlotData(
			INDEX_NONE, Inventory, Address, Item, false);
	}
}

void UInventoryBackpackWidget::UnbindInventory()
{
	if (Inventory && InventoryChangedHandle.IsValid())
	{
		Inventory->OnInventoryChanged().Remove(InventoryChangedHandle);
	}
	InventoryChangedHandle.Reset();
}

void UInventoryBackpackWidget::NativeDestruct()
{
	UnbindInventory();
	Super::NativeDestruct();
}
