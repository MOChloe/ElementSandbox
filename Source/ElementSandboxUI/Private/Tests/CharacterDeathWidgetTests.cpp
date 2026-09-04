#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Vitals/CharacterDeathWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterDeathWidgetStateTest,
	"ElementSandbox.UI.CharacterDeathWidgetState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterDeathWidgetStateTest::RunTest(const FString& Parameters)
{
	UCharacterDeathWidget* Widget = NewObject<UCharacterDeathWidget>();
	if (!TestNotNull(TEXT("可创建纯 C++ 死亡界面"), Widget))
	{
		return false;
	}

	Widget->SetDeathShown(false);
	TestFalse(TEXT("关闭死亡提示时状态为 false"), Widget->IsDeathShown());
	TestTrue(TEXT("关闭死亡提示时折叠"),
		Widget->GetVisibility() == ESlateVisibility::Collapsed);

	Widget->SetDeathShown(true);
	TestTrue(TEXT("死亡后显示提示"), Widget->IsDeathShown());
	TestTrue(TEXT("死亡提示不拦截 R 键输入"),
		Widget->GetVisibility() == ESlateVisibility::HitTestInvisible);

	Widget->SetDeathShown(false);
	TestFalse(TEXT("重生后关闭提示"), Widget->IsDeathShown());
	TestTrue(TEXT("重生后重新折叠"),
		Widget->GetVisibility() == ESlateVisibility::Collapsed);
	return true;
}

#endif
