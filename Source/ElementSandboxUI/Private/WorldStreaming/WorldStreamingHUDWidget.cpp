#include "WorldStreaming/WorldStreamingHUDWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Style/ElementSandboxUIStyle.h"

namespace
{
	FText Number(const int64 Value)
	{
		return FText::AsNumber(Value);
	}
}

void UWorldStreamingHUDWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(
		UCanvasPanel::StaticClass(), TEXT("Canvas"));
	WidgetTree->RootWidget = Canvas;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), TEXT("WorldStreamingPanel"));
	Panel->SetPadding(FMargin(12.0f, 9.0f));
	Panel->SetBrushColor(ElementSandbox::UIStyle::PanelBackground);
	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Panel);
	PanelSlot->SetAnchors(FAnchors(1.0f, 0.0f));
	PanelSlot->SetAlignment(FVector2D(1.0f, 0.0f));
	PanelSlot->SetPosition(FVector2D(-18.0f, 18.0f));
	PanelSlot->SetSize(FVector2D(590.0f, 340.0f));

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(
		UVerticalBox::StaticClass(), TEXT("WorldStreamingContent"));
	Panel->SetContent(Content);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("WorldStreamingTitle"));
	Title->SetText(NSLOCTEXT("ElementSandboxUI", "WorldStreamingTitle", "WORLD STORAGE / CHUNK STREAMING"));
	Title->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::PrimaryText));
	FSlateFontInfo TitleFont = Title->GetFont();
	TitleFont.Size = ElementSandbox::UIStyle::BodyFontSize;
	Title->SetFont(TitleFont);
		Content->AddChildToVerticalBox(Title);

	MetricsText = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("WorldStreamingMetrics"));
	MetricsText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
	FSlateFontInfo MetricsFont = MetricsText->GetFont();
	MetricsFont.Size = ElementSandbox::UIStyle::SmallFontSize;
	MetricsText->SetFont(MetricsFont);
		UVerticalBoxSlot* MetricsSlot = Content->AddChildToVerticalBox(MetricsText);
		MetricsSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));

		LoadingCurtain = WidgetTree->ConstructWidget<UBorder>(
			UBorder::StaticClass(), TEXT("WorldLoadingCurtain"));
		LoadingCurtain->SetBrushColor(ElementSandbox::UIStyle::LoadingBackdrop);
		LoadingCurtain->SetHorizontalAlignment(HAlign_Center);
		LoadingCurtain->SetVerticalAlignment(VAlign_Center);
		UCanvasPanelSlot* CurtainSlot = Canvas->AddChildToCanvas(LoadingCurtain);
		CurtainSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		CurtainSlot->SetOffsets(FMargin(0.0f));
		CurtainSlot->SetZOrder(100);

		USizeBox* LoadingWidth = WidgetTree->ConstructWidget<USizeBox>(
			USizeBox::StaticClass(), TEXT("WorldLoadingWidth"));
		LoadingWidth->SetWidthOverride(460.0f);
		LoadingCurtain->SetContent(LoadingWidth);

		UVerticalBox* LoadingContent = WidgetTree->ConstructWidget<UVerticalBox>(
			UVerticalBox::StaticClass(), TEXT("WorldLoadingContent"));
		LoadingWidth->SetContent(LoadingContent);

		UTextBlock* LoadingBrand = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), TEXT("WorldLoadingBrand"));
		LoadingBrand->SetText(NSLOCTEXT("ElementSandboxUI", "WorldLoadingBrand", "ELEMENT SANDBOX"));
		LoadingBrand->SetJustification(ETextJustify::Center);
		LoadingBrand->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::LoadingAccent));
		FSlateFontInfo BrandFont = LoadingBrand->GetFont();
		BrandFont.Size = ElementSandbox::UIStyle::BodyFontSize;
		LoadingBrand->SetFont(BrandFont);
		LoadingContent->AddChildToVerticalBox(LoadingBrand);

		USpacer* BrandSpacer = WidgetTree->ConstructWidget<USpacer>(
			USpacer::StaticClass(), TEXT("WorldLoadingBrandSpacer"));
		BrandSpacer->SetSize(FVector2D(1.0f, 18.0f));
		LoadingContent->AddChildToVerticalBox(BrandSpacer);

		LoadingStatusText = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), TEXT("WorldLoadingStatus"));
		LoadingStatusText->SetJustification(ETextJustify::Center);
		LoadingStatusText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::PrimaryText));
		FSlateFontInfo LoadingStatusFont = LoadingStatusText->GetFont();
		LoadingStatusFont.Size = 28;
		LoadingStatusText->SetFont(LoadingStatusFont);
		LoadingContent->AddChildToVerticalBox(LoadingStatusText);

		LoadingDetailText = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), TEXT("WorldLoadingDetail"));
		LoadingDetailText->SetJustification(ETextJustify::Center);
		LoadingDetailText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
		FSlateFontInfo LoadingDetailFont = LoadingDetailText->GetFont();
		LoadingDetailFont.Size = ElementSandbox::UIStyle::BodyFontSize;
		LoadingDetailText->SetFont(LoadingDetailFont);
		UVerticalBoxSlot* DetailSlot = LoadingContent->AddChildToVerticalBox(LoadingDetailText);
		DetailSlot->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 22.0f));

		LoadingProgressBar = WidgetTree->ConstructWidget<UProgressBar>(
			UProgressBar::StaticClass(), TEXT("WorldLoadingProgressBar"));
		LoadingProgressBar->SetFillColorAndOpacity(ElementSandbox::UIStyle::LoadingAccent);
		LoadingContent->AddChildToVerticalBox(LoadingProgressBar);

		LoadingProgressText = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), TEXT("WorldLoadingProgress"));
		LoadingProgressText->SetJustification(ETextJustify::Center);
		LoadingProgressText->SetColorAndOpacity(FSlateColor(ElementSandbox::UIStyle::SecondaryText));
		FSlateFontInfo LoadingProgressFont = LoadingProgressText->GetFont();
		LoadingProgressFont.Size = ElementSandbox::UIStyle::SmallFontSize;
		LoadingProgressText->SetFont(LoadingProgressFont);
		UVerticalBoxSlot* ProgressTextSlot = LoadingContent->AddChildToVerticalBox(LoadingProgressText);
		ProgressTextSlot->SetPadding(FMargin(0.0f, 9.0f, 0.0f, 0.0f));
		Refresh();
}

void UWorldStreamingHUDWidget::SetMetrics(const FWorldStreamingHUDMetrics& InMetrics)
{
	Metrics = InMetrics;
	Refresh();
}

void UWorldStreamingHUDWidget::Refresh()
{
	const bool bHasWorldDescription = Metrics.CompleteStructureCount > 0
		|| Metrics.BuildingEntityCount > 0
		|| Metrics.WorldObjectEntityCount > 0
		|| Metrics.ActivationCoreChunkCount > 0
		|| Metrics.OfferedChunkCount > 0
		|| Metrics.ResidentChunkCount > 0
		|| Metrics.AuthorityResidentChunkCount > 0;
	LoadingState = Metrics.bActivationCoreReady
		? EWorldStreamingLoadingState::Ready
		: (bHasWorldDescription
			? EWorldStreamingLoadingState::Loading
			: EWorldStreamingLoadingState::Connecting);
	if (LoadingState == EWorldStreamingLoadingState::Ready)
	{
		LoadingProgress = 1.0f;
	}
	else if (Metrics.ActivationCoreChunkCount > 0)
	{
		const float CoreCount = static_cast<float>(Metrics.ActivationCoreChunkCount);
		const float ClientProgress = static_cast<float>(
			FMath::Clamp(Metrics.ActivationCoreAcknowledgedChunkCount, 0,
				Metrics.ActivationCoreChunkCount)) / CoreCount;
		const float AuthorityProgress = static_cast<float>(
				FMath::Clamp(Metrics.ActivationCoreAuthorityReadyChunkCount, 0,
				Metrics.ActivationCoreChunkCount)) / CoreCount;
		LoadingProgress = FMath::Min(ClientProgress, AuthorityProgress);
	}
	else
	{
		LoadingProgress = 0.0f;
	}
	RefreshLoadingCurtain();

	if (!MetricsText)
	{
		return;
	}
	const uint64 CacheTotal = Metrics.CacheHitCount + Metrics.CacheMissCount;
	const double CacheHitPercent = CacheTotal > 0
		? static_cast<double>(Metrics.CacheHitCount) * 100.0 / CacheTotal
		: 0.0;
	const double ReceiveMiB = static_cast<double>(Metrics.PayloadBytesReceived) / 1048576.0;
	const double SendMiB = static_cast<double>(Metrics.PayloadBytesSent) / 1048576.0;
	const double WorldHours = static_cast<double>(Metrics.WorldSimulationTimeMilliseconds) / 3600000.0;

	MetricsText->SetText(FText::Format(
			NSLOCTEXT("ElementSandboxUI", "WorldStreamingMetricsFormat",
						"Structures {0}  |  Building {1}  |  WorldObject {31}\n"
							"Resident trees {32}  |  All objects {33}\n"
							"Tree A {34}, T {35}, G {36}, P {37}\n"
								"Tree HISM cells {38}, instances {39}  |  Collision {40}  |  Host {41}\n"
								"Tree Build {42}, merged {43}  |  Select {44} ms, Apply {45} ms, Build {46} ms\n"
								"Tree Select L/F {47}/{48}  |  Worker {49}, drop {50}  |  candidates {51}, cell delta {52}\n"
									"Tree Ops add/remove {53}/{54}, invalid-visible {58}  |  Collision submit/query/test {55}/{56}/{57}\n"
					"Activation Core {2}  |  C {3}/{4}, S {5}/{4}\n"
					"Interest Center ({28}, {29}, {30})\n"
					"Snapshot ACK {6}/{7}  |  Pending {16}, segments {22}\n"
					"Object updates pending {59}\n"
					"Resident C {8}/{9}  |  S {10}/{11} (entities/chunks)\n"
						"Pending C I/O {12}, inject {13}  |  S I/O {14}, inject {15}\n"
						"Cache {17}/{18} ({19}%)  |  RX {20} MiB, TX {21} MiB\n"
						"Authority 8 Hz  |  World {23} h  |  Dirty {24}  |  Checkpoint {25}\n"
						"Awake Physics Pin {26}  |  Oldest {27} s"),
		Number(Metrics.CompleteStructureCount),
		Number(Metrics.BuildingEntityCount),
		Metrics.bActivationCoreReady
			? NSLOCTEXT("ElementSandboxUI", "WorldStreamingReady", "READY")
			: NSLOCTEXT("ElementSandboxUI", "WorldStreamingLoading", "LOADING"),
			Number(Metrics.ActivationCoreAcknowledgedChunkCount),
			Number(Metrics.ActivationCoreChunkCount),
				Number(Metrics.ActivationCoreAuthorityReadyChunkCount),
			Number(Metrics.AcknowledgedChunkCount),
			Number(Metrics.OfferedChunkCount),
				Number(Metrics.ResidentEntityCount),
			Number(Metrics.ResidentChunkCount),
			Number(Metrics.AuthorityResidentEntityCount),
			Number(Metrics.AuthorityResidentChunkCount),
			Number(Metrics.PendingLoadCount),
			Number(Metrics.PendingInjectionCount),
				Number(Metrics.AuthorityPendingLoadCount),
				Number(Metrics.AuthorityPendingInjectionCount),
			Number(Metrics.NetworkPendingChunkCount),
			Number(Metrics.CacheHitCount),
		Number(CacheTotal),
		FText::AsNumber(CacheHitPercent, &FNumberFormattingOptions().SetMaximumFractionalDigits(1)),
		FText::AsNumber(ReceiveMiB, &FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
		FText::AsNumber(SendMiB, &FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
		Number(Metrics.SegmentsInFlight),
		FText::AsNumber(WorldHours, &FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
		Number(Metrics.DirtyEntityCount),
			Metrics.bCheckpointInFlight
				? NSLOCTEXT("ElementSandboxUI", "WorldStreamingSaving", "WRITING")
				: NSLOCTEXT("ElementSandboxUI", "WorldStreamingIdle", "IDLE"),
			Number(Metrics.AuthorityAwakePhysicsPinnedEntityCount),
				FText::AsNumber(
					Metrics.AuthorityOldestAwakePhysicsPinSeconds,
					&FNumberFormattingOptions().SetMaximumFractionalDigits(1)),
				Number(Metrics.InterestCenterX),
				Number(Metrics.InterestCenterY),
					Number(Metrics.InterestCenterZ),
					Number(Metrics.WorldObjectEntityCount),
					Number(Metrics.TreeResidentCount),
					Number(Metrics.WorldObjectResidentEntityCount),
					Number(Metrics.TreeActiveCount),
					Number(Metrics.TreeTransitionCount),
					Number(Metrics.TreeGraceCount),
					Number(Metrics.TreePendingCount),
						Number(Metrics.TreeHISMCellCount),
					Number(Metrics.TreeInstanceCount),
						Number(Metrics.TreeCollisionCount),
						Number(Metrics.TreeRenderHostCount),
						Number(Metrics.TreeBuildCount),
						Number(Metrics.TreeCoalescedBuildCount),
						FText::AsNumber(Metrics.TreeSelectionMilliseconds,
							&FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
						FText::AsNumber(Metrics.TreeApplyMilliseconds,
							&FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
							FText::AsNumber(Metrics.TreeBuildMilliseconds,
									&FNumberFormattingOptions().SetMaximumFractionalDigits(2)),
							Number(Metrics.TreeLocalSelectionPassCount),
						Number(Metrics.TreeFarSelectionPassCount),
						Number(Metrics.TreeWorkerDispatchCount),
						Number(Metrics.TreeWorkerDiscardCount),
						Number(Metrics.TreeCandidateTestCount),
						Number(Metrics.TreeCellDeltaEvaluationCount),
							Number(Metrics.TreeHISMAddCount),
							Number(Metrics.TreeHISMRemoveCount),
							Number(Metrics.TreeCollisionSourceSubmitCount),
							Number(Metrics.TreeCollisionCatalogQueryCount),
							Number(Metrics.TreeCollisionCandidateTestCount),
							Number(Metrics.TreeInvalidVisibleRemovalCount),
							Number(Metrics.NetworkPendingLiveDeltaCount)));
}

void UWorldStreamingHUDWidget::RefreshLoadingCurtain()
{
	if (!LoadingCurtain)
	{
		return;
	}
	const bool bReady = LoadingState == EWorldStreamingLoadingState::Ready;
	LoadingCurtain->SetVisibility(bReady
		? ESlateVisibility::Collapsed
		: ESlateVisibility::HitTestInvisible);
	if (bReady || !LoadingStatusText || !LoadingDetailText
		|| !LoadingProgressBar || !LoadingProgressText)
	{
		return;
	}

	const bool bConnecting = LoadingState == EWorldStreamingLoadingState::Connecting;
	LoadingStatusText->SetText(bConnecting
		? NSLOCTEXT("ElementSandboxUI", "WorldLoadingConnecting", "正在连接服务器")
		: NSLOCTEXT("ElementSandboxUI", "WorldLoadingWorld", "正在加载世界"));
	LoadingDetailText->SetText(bConnecting
		? NSLOCTEXT("ElementSandboxUI", "WorldLoadingConnectingDetail",
			"正在建立本地服务器会话与世界流送通道")
		: NSLOCTEXT("ElementSandboxUI", "WorldLoadingWorldDetail",
			"正在装填附近场景，完成后将自动进入游戏"));
	LoadingProgressBar->SetVisibility(bConnecting
		? ESlateVisibility::Collapsed
		: ESlateVisibility::HitTestInvisible);
	LoadingProgressBar->SetPercent(LoadingProgress);
	LoadingProgressText->SetText(bConnecting
		? NSLOCTEXT("ElementSandboxUI", "WorldLoadingPleaseWait", "请稍候…")
		: FText::Format(
			NSLOCTEXT("ElementSandboxUI", "WorldLoadingCoreProgress",
				"客户端 {0}/{1}  ·  服务器 {2}/{1}"),
			Number(Metrics.ActivationCoreAcknowledgedChunkCount),
			Number(Metrics.ActivationCoreChunkCount),
				Number(Metrics.ActivationCoreAuthorityReadyChunkCount)));
}
