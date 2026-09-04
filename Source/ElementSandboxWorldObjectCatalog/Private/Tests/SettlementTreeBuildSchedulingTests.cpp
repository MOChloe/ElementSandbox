#if WITH_DEV_AUTOMATION_TESTS

#include "Tree/SettlementTreeDefinition.h"
#include "Tree/SettlementTreePresentationWorldSubsystem.h"
#include "Tree/SettlementTreeSettings.h"
#include "Tree/SettlementTreeWorldSubsystem.h"

#include "Async/TaskGraphInterfaces.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "PresentationWorldSubsystem.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettlementTreeBuildDuringOtherCellInjectionTest,
	"ElementSandbox.WorldObjects.Tree.BuildDuringOtherCellInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSettlementTreeBuildDuringOtherCellInjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game, false, TEXT("SettlementTreeBuildDuringOtherCellInjection"), nullptr, true);
	if (!TestNotNull(TEXT("创建树木持续装填测试 World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	UPresentationWorldSubsystem* Presentation = World->GetSubsystem<UPresentationWorldSubsystem>();
	USettlementTreePresentationWorldSubsystem* Trees =
		World->GetSubsystem<USettlementTreePresentationWorldSubsystem>();
	USettlementTreeWorldSubsystem* Catalog = World->GetSubsystem<USettlementTreeWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!TestTrue(TEXT("持续装填测试依赖可用"), Presentation && Trees && Catalog && WorldObjects))
	{
		return false;
	}

	USettlementTreeSettings* Settings = GetMutableDefault<USettlementTreeSettings>();
	const double SavedQuietSeconds = Settings->TreeBuildQuietSeconds;
	Settings->TreeBuildQuietSeconds = 0.05;
	ON_SCOPE_EXIT { Settings->TreeBuildQuietSeconds = SavedQuietSeconds; };

	FPresentationViewSource View;
	View.ViewLocation = FVector::ZeroVector;
	View.SubjectLocation = FVector::ZeroVector;
	View.Forward = FVector::ForwardVector;
	View.Right = FVector::RightVector;
	View.Up = FVector::UpVector;
	View.HorizontalFOVDegrees = 90.0f;
	View.AspectRatio = 16.0f / 9.0f;
	View.ViewportSize = FIntPoint(1280, 720);
	View.Revision = 1;
	const FPresentationSourceHandle Source = Presentation->RegisterSource(View);
	ON_SCOPE_EXIT { Presentation->UnregisterSource(Source); };

	const double SelectionDeadline = FPlatformTime::Seconds() + 3.0;
	while (!Trees->IsIdle() && FPlatformTime::Seconds() < SelectionDeadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Trees->Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	if (!TestTrue(TEXT("空 Catalog 的选择已稳定"), Trees->IsIdle()))
	{
		return false;
	}

	const auto AddTree = [&](const FVector& Location)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Catalog->GetDefinition();
		Desc.WorldTransform = FTransform(Location);
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		const bool bCreated = WorldObjects->CreateEntity(Desc).IsSet();
		Catalog->Tick(0.0f);
		return bCreated;
	};

	bool bResult = TestTrue(TEXT("第一个 Cell 已注入一棵树"), AddTree(FVector(50000.0, 5000.0, 0.0)));
	Trees->Tick(0.0f);
	bResult &= TestEqual(TEXT("已装入 HISM，但尚未到首次建树时间"), Trees->GetStats().InstanceCount, 1);
	const int64 InitialBuildCount = Trees->GetStats().TreeBuildCount;

	// 每帧在另一个 1km Cell 装填，把它的截止时间一直留在未来。
	// 已静默的第一个 Cell 必须照常建树，不能等待整个世界停止注入。
	int32 InjectedTreeCount = 0;
	const double InjectionDeadline = FPlatformTime::Seconds() + 0.3;
	while (FPlatformTime::Seconds() < InjectionDeadline)
	{
		bResult &= AddTree(FVector(150000.0, 5000.0 + InjectedTreeCount * 10.0, 0.0));
		++InjectedTreeCount;
		Trees->Tick(0.005f);
		FPlatformProcess::Sleep(0.005f);
	}
	bResult &= TestTrue(TEXT("另一个 Cell 持续接收多批树木"), InjectedTreeCount > 1);
	bResult &= TestTrue(TEXT("未来任务不能挡住已到期 Cell 的首次建树"),
		Trees->GetStats().TreeBuildCount > InitialBuildCount);
	return bResult;
}

#endif
