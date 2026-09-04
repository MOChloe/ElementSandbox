#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "WorldStreaming/WorldStreamingHUDWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStreamingHUDLoadingStateTest,
	"ElementSandbox.UI.WorldStreamingHUDLoadingState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStreamingHUDLoadingStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorldStreamingHUDWidget* Widget = NewObject<UWorldStreamingHUDWidget>();
	if (!TestNotNull(TEXT("可创建纯 C++ 世界流送 HUD"), Widget))
	{
		return false;
	}

	TestTrue(TEXT("尚未收到世界信息时显示连接遮罩"),
		Widget->GetLoadingState() == EWorldStreamingLoadingState::Connecting);
	TestTrue(TEXT("连接阶段进度为零"),
		FMath::IsNearlyZero(Widget->GetLoadingProgress()));

	FWorldStreamingHUDMetrics Metrics;
	Metrics.CompleteStructureCount = 1000000;
	Metrics.BuildingEntityCount = 31254200;
	Metrics.WorldObjectEntityCount = 1000000;
	Metrics.ActivationCoreChunkCount = 6;
	Metrics.ActivationCoreAcknowledgedChunkCount = 2;
	Metrics.ActivationCoreAuthorityReadyChunkCount = 4;
	Widget->SetMetrics(Metrics);
	TestTrue(TEXT("已收到 Manifest 但核心未就绪时显示加载遮罩"),
		Widget->GetLoadingState() == EWorldStreamingLoadingState::Loading);
	TestTrue(TEXT("加载进度取客户端与服务器完成度较小值"),
		FMath::IsNearlyEqual(Widget->GetLoadingProgress(), 2.0f / 6.0f));

	Metrics.bActivationCoreReady = true;
	Widget->SetMetrics(Metrics);
	TestTrue(TEXT("Activation Core 就绪后关闭加载遮罩"),
		Widget->GetLoadingState() == EWorldStreamingLoadingState::Ready);
	TestTrue(TEXT("就绪后进度为一"),
		FMath::IsNearlyEqual(Widget->GetLoadingProgress(), 1.0f));
	return true;
}

#endif
