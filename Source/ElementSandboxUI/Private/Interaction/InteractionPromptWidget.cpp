#include "Interaction/InteractionPromptWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Style/ElementSandboxUIStyle.h"

void UInteractionPromptWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("PromptBackground"));
	Background->SetPadding(FMargin(10.0f, 6.0f));
	Background->SetBrushColor(ElementSandbox::UIStyle::PanelBackground);
	WidgetTree->RootWidget = Background;

	PromptTextBlock = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("PromptText"));
	PromptTextBlock->SetColorAndOpacity(
		FSlateColor(ElementSandbox::UIStyle::PrimaryText));
	PromptTextBlock->SetJustification(ETextJustify::Center);
	FSlateFontInfo Font = PromptTextBlock->GetFont();
	Font.Size = ElementSandbox::UIStyle::BodyFontSize;
	PromptTextBlock->SetFont(Font);
	Background->SetContent(PromptTextBlock);

	RefreshText();
}

void UInteractionPromptWidget::SetPromptText(const FText& InText)
{
	PromptText = InText;
	RefreshText();
}

void UInteractionPromptWidget::RefreshText()
{
	if (PromptTextBlock)
	{
		PromptTextBlock->SetText(PromptText);
	}
}
