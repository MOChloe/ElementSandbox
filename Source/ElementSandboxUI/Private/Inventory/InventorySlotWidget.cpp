#include "Inventory/InventorySlotWidget.h"

#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryDragDropOperation.h"
#include "Item/Features/ItemDisplayFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemInstance.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "InputCoreTypes.h"
#include "Style/ElementSandboxUIStyle.h"

void UInventorySlotWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	SlotSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("SlotSize"));
	SlotSize->SetWidthOverride(64.0f);
	SlotSize->SetHeightOverride(64.0f);
	WidgetTree->RootWidget = SlotSize;

	FrameBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("FrameBorder"));
	FrameBorder->SetPadding(FMargin(2.0f));
	FrameBorder->SetBrushColor(ElementSandbox::UIStyle::IdleFrame);
	SlotSize->SetContent(FrameBorder);

	SlotBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("SlotBorder"));
	SlotBorder->SetPadding(FMargin(0.0f));
	SlotBorder->SetBrushColor(ElementSandbox::UIStyle::IdleSurface);
	FrameBorder->SetContent(SlotBorder);

	UOverlay* ContentOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("ContentOverlay"));
	ContentOverlay->SetClipping(EWidgetClipping::ClipToBounds);
	SlotBorder->SetContent(ContentOverlay);

	ShortcutText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ShortcutText"));
	ShortcutText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	FSlateFontInfo ShortcutFont = ShortcutText->GetFont();
	ShortcutFont.Size = ElementSandbox::UIStyle::SmallFontSize;
	ShortcutText->SetFont(ShortcutFont);
	UOverlaySlot* ShortcutSlot = ContentOverlay->AddChildToOverlay(ShortcutText);
	ShortcutSlot->SetHorizontalAlignment(HAlign_Left);
	ShortcutSlot->SetVerticalAlignment(VAlign_Top);
	ShortcutSlot->SetPadding(FMargin(5.0f, 2.0f, 0.0f, 0.0f));

	ItemText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ItemText"));
	ItemText->SetJustification(ETextJustify::Center);
	ItemText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::PrimaryText));
	ItemText->SetAutoWrapText(false);
	FSlateFontInfo ItemFont = ItemText->GetFont();
	ItemFont.Size = ElementSandbox::UIStyle::BodyFontSize;
	ItemText->SetFont(ItemFont);
	UOverlaySlot* ItemSlot = ContentOverlay->AddChildToOverlay(ItemText);
	ItemSlot->SetHorizontalAlignment(HAlign_Fill);
	ItemSlot->SetVerticalAlignment(VAlign_Center);
	ItemSlot->SetPadding(FMargin(4.0f, 14.0f, 4.0f, 4.0f));

	QuantityText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("QuantityText"));
	QuantityText->SetJustification(ETextJustify::Right);
	QuantityText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	FSlateFontInfo QuantityFont = QuantityText->GetFont();
	QuantityFont.Size = ElementSandbox::UIStyle::SmallFontSize;
	QuantityText->SetFont(QuantityFont);
	UOverlaySlot* QuantitySlot = ContentOverlay->AddChildToOverlay(QuantityText);
	QuantitySlot->SetHorizontalAlignment(HAlign_Right);
	QuantitySlot->SetVerticalAlignment(VAlign_Bottom);
	QuantitySlot->SetPadding(FMargin(0.0f, 0.0f, 4.0f, 2.0f));
}

void UInventorySlotWidget::SetSlotData(
	const int32 ShortcutNumber,
	UInventoryComponent* InInventory,
	const FInventorySlotAddress& InAddress,
	UItemInstance* ItemInstance,
	const bool bSelected)
{
	Inventory = InInventory;
	Address = InAddress;
	CurrentItem = ItemInstance;

	if (!SlotSize || !FrameBorder || !SlotBorder || !ShortcutText || !ItemText || !QuantityText)
	{
		return;
	}

	const bool bQuickbarSlot = ShortcutNumber >= 0;
	SlotSize->SetWidthOverride(bQuickbarSlot ? 64.0f : 70.0f);
	SlotSize->SetHeightOverride(bQuickbarSlot ? 64.0f : 70.0f);
	FrameBorder->SetBrushColor(bSelected
		? ElementSandbox::UIStyle::SelectedFrame
		: ElementSandbox::UIStyle::IdleFrame);
	SlotBorder->SetBrushColor(bSelected
		? ElementSandbox::UIStyle::SelectedSurface
		: ElementSandbox::UIStyle::IdleSurface);

	if (ShortcutNumber >= 0)
	{
		const int32 DisplayNumber = ShortcutNumber == 9 ? 0 : ShortcutNumber + 1;
		ShortcutText->SetText(FText::AsNumber(DisplayNumber));
		ShortcutText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	else
	{
		ShortcutText->SetText(FText::GetEmpty());
		ShortcutText->SetVisibility(ESlateVisibility::Collapsed);
	}

	FText DisplayName = FText::GetEmpty();
	FText Quantity = FText::GetEmpty();
	bool bShowQuantity = false;
	if (IsValid(ItemInstance))
	{
		if (const UItemDisplayFeature* Display = ItemInstance->FindFeature<UItemDisplayFeature>())
		{
			DisplayName = Display->DisplayName;
		}
		if (const UItemStackFeature* Stack = ItemInstance->FindFeature<UItemStackFeature>())
		{
			bShowQuantity = Stack->GetQuantity() > 1;
			if (bShowQuantity)
			{
				Quantity = FText::Format(NSLOCTEXT("ElementSandbox", "ItemQuantity", "x{0}"), FText::AsNumber(Stack->GetQuantity()));
			}
		}
	}

	ItemText->SetText(DisplayName);
	QuantityText->SetText(Quantity);
	QuantityText->SetVisibility(bShowQuantity ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

FReply UInventorySlotWidget::NativeOnMouseButtonDown(
	const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (IsValid(CurrentItem) && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		return UWidgetBlueprintLibrary::DetectDragIfPressed(
			InMouseEvent, this, EKeys::LeftMouseButton).NativeReply;
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UInventorySlotWidget::NativeOnDragDetected(
	const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent,
	UDragDropOperation*& OutOperation)
{
	Super::NativeOnDragDetected(InGeometry, InMouseEvent, OutOperation);
	if (!Inventory || !IsValid(CurrentItem))
	{
		return;
	}

	UInventoryDragDropOperation* DragOperation = NewObject<UInventoryDragDropOperation>();
	DragOperation->Inventory = Inventory;
	DragOperation->SourceAddress = Address;
	DragOperation->Pivot = EDragPivot::CenterCenter;

	UInventorySlotWidget* DragVisual = CreateWidget<UInventorySlotWidget>(
		GetOwningPlayer(), UInventorySlotWidget::StaticClass());
	if (DragVisual)
	{
		DragVisual->SetSlotData(INDEX_NONE, nullptr, FInventorySlotAddress(), CurrentItem, false);
		DragVisual->SetRenderOpacity(0.85f);
		DragOperation->DefaultDragVisual = DragVisual;
	}
	OutOperation = DragOperation;
}

bool UInventorySlotWidget::NativeOnDrop(
	const FGeometry& InGeometry,
	const FDragDropEvent& InDragDropEvent,
	UDragDropOperation* InOperation)
{
	const UInventoryDragDropOperation* ItemOperation = Cast<UInventoryDragDropOperation>(InOperation);
	if (!ItemOperation || !Inventory || ItemOperation->Inventory != Inventory
		|| (ItemOperation->SourceAddress.Container == Address.Container
			&& ItemOperation->SourceAddress.Index == Address.Index))
	{
		return Super::NativeOnDrop(InGeometry, InDragDropEvent, InOperation);
	}

	Inventory->RequestMoveItem(ItemOperation->SourceAddress, Address);
	return true;
}
