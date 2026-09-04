#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Interaction/InteractionPromptWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInteractionPromptWidgetStateTest,
	"ElementSandbox.UI.InteractionPromptWidgetState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInteractionPromptWidgetStateTest::RunTest(const FString& Parameters)
{
	UInteractionPromptWidget* Widget = NewObject<UInteractionPromptWidget>();
	if (!TestNotNull(TEXT("可创建纯 C++ 交互提示 Widget"), Widget))
	{
		return false;
	}

	const FText Prompt = FText::FromString(TEXT("按 E 拾取"));
	Widget->SetPromptText(Prompt);
	TestEqual(TEXT("Widget 只保存外部提交的文字"),
		Widget->GetPromptText().ToString(), Prompt.ToString());
	return true;
}

#endif
