#include "Vitals/CharacterHealthBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Style/ElementSandboxUIStyle.h"

void UCharacterHealthBarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(
		UCanvasPanel::StaticClass(), TEXT("Canvas"));
	WidgetTree->RootWidget = Canvas;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("HealthPanel"));
	Panel->SetPadding(FMargin(5.0f));
	Panel->SetBrushColor(ElementSandbox::UIStyle::PanelBackground);
	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Panel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 1.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 1.0f));
	PanelSlot->SetPosition(FVector2D(28.0f, -18.0f));
	PanelSlot->SetSize(FVector2D(280.0f, 74.0f));

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("HealthFrame"));
	Frame->SetPadding(FMargin(2.0f));
	Frame->SetBrushColor(ElementSandbox::UIStyle::IdleFrame);
	Panel->SetContent(Frame);

	UBorder* Surface = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("HealthSurface"));
	Surface->SetPadding(FMargin(10.0f, 7.0f));
	Surface->SetBrushColor(ElementSandbox::UIStyle::IdleSurface);
	Frame->SetContent(Surface);

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(
		UVerticalBox::StaticClass(), TEXT("HealthContent"));
	Surface->SetContent(Content);

	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>(
		UHorizontalBox::StaticClass(), TEXT("HealthHeader"));
	Content->AddChildToVerticalBox(Header);

	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("HealthLabel"));
	Label->SetText(NSLOCTEXT("ElementSandboxUI", "CharacterHealthLabel", "生命"));
	Label->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	FSlateFontInfo LabelFont = Label->GetFont();
	LabelFont.Size = ElementSandbox::UIStyle::SmallFontSize;
	Label->SetFont(LabelFont);
	UHorizontalBoxSlot* LabelSlot = Header->AddChildToHorizontalBox(Label);
	LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
	LabelSlot->SetVerticalAlignment(VAlign_Center);

	HealthValueText = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("HealthValueText"));
	HealthValueText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::PrimaryText));
	HealthValueText->SetJustification(ETextJustify::Right);
	FSlateFontInfo ValueFont = HealthValueText->GetFont();
	ValueFont.Size = ElementSandbox::UIStyle::BodyFontSize;
	HealthValueText->SetFont(ValueFont);
	UHorizontalBoxSlot* ValueSlot = Header->AddChildToHorizontalBox(HealthValueText);
	ValueSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ValueSlot->SetHorizontalAlignment(HAlign_Fill);
	ValueSlot->SetVerticalAlignment(VAlign_Center);

	USizeBox* ProgressSize = WidgetTree->ConstructWidget<USizeBox>(
		USizeBox::StaticClass(), TEXT("HealthProgressSize"));
	ProgressSize->SetHeightOverride(12.0f);
	UVerticalBoxSlot* ProgressSizeSlot = Content->AddChildToVerticalBox(ProgressSize);
	ProgressSizeSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));

	HealthProgressBar = WidgetTree->ConstructWidget<UProgressBar>(
		UProgressBar::StaticClass(), TEXT("HealthProgressBar"));
	FProgressBarStyle ProgressStyle = HealthProgressBar->GetWidgetStyle();
	ProgressStyle.BackgroundImage.TintColor = FSlateColor(ElementSandbox::UIStyle::HealthTrack);
	ProgressStyle.FillImage.TintColor = FSlateColor(FLinearColor::White);
	HealthProgressBar->SetWidgetStyle(ProgressStyle);
	HealthProgressBar->SetBarFillStyle(EProgressBarFillStyle::Scale);
	HealthProgressBar->SetFillColorAndOpacity(ElementSandbox::UIStyle::HealthFill);
	ProgressSize->SetContent(HealthProgressBar);

	Refresh();
}

void UCharacterHealthBarWidget::SetHealth(
	const float InHealth,
	const float InMaxHealth)
{
	DisplayedMaxHealth = FMath::Max(0.0f, InMaxHealth);
	DisplayedHealth = FMath::Clamp(InHealth, 0.0f, DisplayedMaxHealth);
	Refresh();
}

float UCharacterHealthBarWidget::GetDisplayedPercent() const
{
	return DisplayedMaxHealth > 0.0f
		? DisplayedHealth / DisplayedMaxHealth
		: 0.0f;
}

void UCharacterHealthBarWidget::Refresh()
{
	if (HealthProgressBar)
	{
		const float HealthPercent = GetDisplayedPercent();
		HealthProgressBar->SetPercent(HealthPercent);
		HealthProgressBar->SetFillColorAndOpacity(
			HealthPercent > 0.0f && HealthPercent <= 0.25f
				? ElementSandbox::UIStyle::CriticalHealthFill
				: ElementSandbox::UIStyle::HealthFill);
	}

	if (HealthValueText)
	{
		const int32 RoundedHealth = FMath::RoundToInt(DisplayedHealth);
		const int32 RoundedMaxHealth = FMath::RoundToInt(DisplayedMaxHealth);
		HealthValueText->SetText(FText::Format(
			NSLOCTEXT("ElementSandboxUI", "CharacterHealthValue", "{0} / {1}"),
			FText::AsNumber(RoundedHealth),
			FText::AsNumber(RoundedMaxHealth)));
	}
}
