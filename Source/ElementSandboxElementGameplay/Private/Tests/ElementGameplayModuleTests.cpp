#if WITH_DEV_AUTOMATION_TESTS

#include "ElementGameplayWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementGameplayModuleLifecycleTest,
	"ElementSandbox.Element.Gameplay.ModuleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementGameplayModuleLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("ElementGameplay 模块已加载"),
		FModuleManager::Get().IsModuleLoaded(TEXT("ElementSandboxElementGameplay")));

	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game, false, TEXT("ElementGameplayModuleLifecycle"), nullptr, true);
	if (!TestNotNull(TEXT("创建 Gameplay World"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UElementGameplayWorldSubsystem* Subsystem = World->GetSubsystem<UElementGameplayWorldSubsystem>();
	TestNotNull(TEXT("Game World 创建 ElementGameplay Subsystem"), Subsystem);
	if (Subsystem)
	{
		TestTrue(TEXT("Game World 已装配生产 Fire Runtime"),
			Subsystem->IsRuntimeAssemblyActive());
	}

	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
