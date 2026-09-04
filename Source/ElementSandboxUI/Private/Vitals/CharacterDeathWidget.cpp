#include "Vitals/CharacterDeathWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Style/ElementSandboxUIStyle.h"

void UCharacterDeathWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(
		UOverlay::StaticClass(), TEXT("DeathOverlay"));
	WidgetTree->RootWidget = Root;

	UBorder* Shade = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("DeathShade"));
	Shade->SetBrushColor(FLinearColor(0.01f, 0.01f, 0.015f, 0.68f));
	UOverlaySlot* ShadeSlot = Root->AddChildToOverlay(Shade);
	ShadeSlot->SetHorizontalAlignment(HAlign_Fill);
	ShadeSlot->SetVerticalAlignment(VAlign_Fill);

	UBorder* MessagePanel = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("DeathMessagePanel"));
	MessagePanel->SetPadding(FMargin(48.0f, 28.0f));
	MessagePanel->SetBrushColor(ElementSandbox::UIStyle::OpaquePanelBackground);
	UOverlaySlot* MessageSlot = Root->AddChildToOverlay(MessagePanel);
	MessageSlot->SetHorizontalAlignment(HAlign_Center);
	MessageSlot->SetVerticalAlignment(VAlign_Center);

	UVerticalBox* Message = WidgetTree->ConstructWidget<UVerticalBox>(
		UVerticalBox::StaticClass(), TEXT("DeathMessage"));
	MessagePanel->SetContent(Message);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("DeathTitle"));
	Title->SetText(NSLOCTEXT("ElementSandboxUI", "CharacterDeathTitle", "你死了"));
	Title->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::CriticalHealthFill));
	Title->SetJustification(ETextJustify::Center);
	FSlateFontInfo TitleFont = Title->GetFont();
	TitleFont.Size = 42;
	Title->SetFont(TitleFont);
	UVerticalBoxSlot* TitleSlot = Message->AddChildToVerticalBox(Title);
	TitleSlot->SetHorizontalAlignment(HAlign_Fill);

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("RespawnHint"));
	Hint->SetText(NSLOCTEXT(
		"ElementSandboxUI", "CharacterRespawnHint", "按 R 在出生点重生"));
	Hint->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	Hint->SetJustification(ETextJustify::Center);
	FSlateFontInfo HintFont = Hint->GetFont();
	HintFont.Size = ElementSandbox::UIStyle::BodyFontSize;
	Hint->SetFont(HintFont);
	UVerticalBoxSlot* HintSlot = Message->AddChildToVerticalBox(Hint);
	HintSlot->SetPadding(FMargin(0.0f, 14.0f, 0.0f, 0.0f));
	HintSlot->SetHorizontalAlignment(HAlign_Fill);

	RefreshVisibility();
}

void UCharacterDeathWidget::SetDeathShown(const bool bShown)
{
	bDeathShown = bShown;
	RefreshVisibility();
}

void UCharacterDeathWidget::RefreshVisibility()
{
	SetVisibility(bDeathShown
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
}
