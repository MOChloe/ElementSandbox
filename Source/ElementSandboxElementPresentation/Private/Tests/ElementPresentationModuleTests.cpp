#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ElementPresentationWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

namespace ElementSandbox::ElementPresentation::Tests
{
static UWorld* CreateInitializedWorld(
	const EWorldType::Type WorldType,
	const FName Name,
	const ENetMode NetMode)
{
	UWorld::InitializationValues Values;
	Values.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(
		WorldType, false, Name, nullptr, true, ERHIFeatureLevel::Num, &Values, true);
	if (!World)
	{
		return nullptr;
	}
	GEngine->CreateNewWorldContext(WorldType).SetCurrentWorld(World);
	if (WorldType == EWorldType::PIE)
	{
		World->SetPlayInEditorInitialNetMode(NetMode);
	}
	World->InitWorld(Values);
	return World;
}
} // namespace ElementSandbox::ElementPresentation::Tests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationModuleLifecycleTest,
	"ElementSandbox.Element.Presentation.ModuleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationModuleLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	TestTrue(TEXT("ElementPresentation 模块已加载"),
		FModuleManager::Get().IsModuleLoaded(TEXT("ElementSandboxElementPresentation")));

	UWorld* World = CreateInitializedWorld(
		EWorldType::Game, TEXT("ElementPresentationModuleLifecycle"), NM_Standalone);
	if (!TestNotNull(TEXT("创建客户端表现 World"), World))
	{
		return false;
	}
	UElementPresentationWorldSubsystem* Subsystem =
		World->GetSubsystem<UElementPresentationWorldSubsystem>();
	TestNotNull(TEXT("Game World 创建独立 ElementPresentation Subsystem"), Subsystem);
	if (Subsystem)
	{
		TestTrue(TEXT("客户端表现状态已初始化"), Subsystem->IsPresentationStateAllocated());
		TestFalse(TEXT("空队列时不 Tick"), Subsystem->IsTickable());
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementPresentationDedicatedServerZeroAllocationTest,
	"ElementSandbox.Element.Presentation.DedicatedServerZeroAllocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementPresentationDedicatedServerZeroAllocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::ElementPresentation::Tests;
	UWorld* World = CreateInitializedWorld(
		EWorldType::PIE, TEXT("ElementPresentationDedicatedServer"), NM_DedicatedServer);
	if (!TestNotNull(TEXT("创建 Dedicated Server World"), World))
	{
		return false;
	}
	TestEqual(TEXT("World NetMode 为 Dedicated Server"), World->GetNetMode(), NM_DedicatedServer);
	TestNull(TEXT("Dedicated Server 不创建 ElementPresentation Subsystem"),
		World->GetSubsystem<UElementPresentationWorldSubsystem>());
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
