#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "WorldStorageProcessRole.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldStorageProcessRoleTest,
	"ElementSandbox.WorldStorage.ProcessRole.ExternalLocalServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStorageProcessRoleTest::RunTest(const FString& Parameters)
{
	FWorldStorageProcessRoleContext Context;
	Context.bGameProcess = true;
	TestTrue(TEXT("裸 -game 客户端默认使用外部本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));

	Context.bGameProcess = false;
	TestFalse(TEXT("没有 PIE Game World 的普通 Editor 会话不启动本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));

	Context.bPlayInEditorStandaloneWorld = true;
	TestTrue(TEXT("普通单玩家 PIE 自动使用隐藏的外部本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	Context.bPlayInEditorStandaloneWorld = false;
	Context.bGameProcess = true;

	Context.bCommandlet = true;
	TestFalse(TEXT("Commandlet 不启动本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	Context.bCommandlet = false;

	Context.bAutomationTesting = true;
	TestFalse(TEXT("Automation 测试 World 保持进程内 Authority，不启动外部本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	Context.bAutomationTesting = false;

	Context.bDedicatedServer = true;
	TestFalse(TEXT("Dedicated Server 不递归启动本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	Context.bDedicatedServer = false;

	Context.bLocalServerChild = true;
	TestFalse(TEXT("本地服务器子进程不递归启动本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	Context.bLocalServerChild = false;

	Context.bLocalServerLaunchSuppressed = true;
	TestFalse(TEXT("已有外部服务器的显式 Client 启动可抑制本地服务器"),
		FWorldStorageProcessRole::ShouldUseExternalLocalServer(Context));
	return true;
}

#endif
