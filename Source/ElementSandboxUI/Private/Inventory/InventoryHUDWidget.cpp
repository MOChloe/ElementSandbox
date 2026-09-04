#include "Inventory/InventoryHUDWidget.h"

#include "Inventory/InventoryBackpackWidget.h"
#include "Inventory/InventoryQuickbarWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"

void UInventoryHUDWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
	WidgetTree->RootWidget = Canvas;

	QuickbarWidget = WidgetTree->ConstructWidget<UInventoryQuickbarWidget>(
		UInventoryQuickbarWidget::StaticClass(), TEXT("Quickbar"));
	UCanvasPanelSlot* QuickbarSlot = Canvas->AddChildToCanvas(QuickbarWidget);
	QuickbarSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	QuickbarSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	QuickbarSlot->SetPosition(FVector2D(0.0f, -18.0f));
	QuickbarSlot->SetAutoSize(true);

	BackpackWidget = WidgetTree->ConstructWidget<UInventoryBackpackWidget>(
		UInventoryBackpackWidget::StaticClass(), TEXT("Backpack"));
	UCanvasPanelSlot* BackpackSlot = Canvas->AddChildToCanvas(BackpackWidget);
	BackpackSlot->SetAnchors(FAnchors(0.0f, 0.5f));
	BackpackSlot->SetAlignment(FVector2D(0.0f, 0.5f));
	BackpackSlot->SetPosition(FVector2D(32.0f, 0.0f));
	BackpackSlot->SetAutoSize(true);

	SetBackpackOpen(false);
}

void UInventoryHUDWidget::SetInventory(UInventoryComponent* InInventory)
{
	if (QuickbarWidget)
	{
		QuickbarWidget->SetInventory(InInventory);
	}
	if (BackpackWidget)
	{
		BackpackWidget->SetInventory(InInventory);
	}
}

void UInventoryHUDWidget::SetBackpackOpen(const bool bOpen)
{
	bBackpackOpen = bOpen;
	if (BackpackWidget)
	{
		BackpackWidget->SetVisibility(bOpen ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}
