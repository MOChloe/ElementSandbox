#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Vitals/CharacterHealthBarWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterHealthBarStateTest,
	"ElementSandbox.UI.CharacterHealthBarState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterHealthBarStateTest::RunTest(const FString& Parameters)
{
	UCharacterHealthBarWidget* Widget = NewObject<UCharacterHealthBarWidget>();
	Widget->SetHealth(75.0f, 100.0f);
	TestEqual(TEXT("正常生命值"), Widget->GetDisplayedHealth(), 75.0f);
	TestEqual(TEXT("正常生命上限"), Widget->GetDisplayedMaxHealth(), 100.0f);
	TestEqual(TEXT("正常生命比例"), Widget->GetDisplayedPercent(), 0.75f);

	Widget->SetHealth(150.0f, 100.0f);
	TestEqual(TEXT("表现层拒绝显示超出上限的生命"), Widget->GetDisplayedHealth(), 100.0f);
	TestEqual(TEXT("超出上限时比例仍为一"), Widget->GetDisplayedPercent(), 1.0f);

	Widget->SetHealth(-10.0f, 0.0f);
	TestEqual(TEXT("无有效上限时生命归零"), Widget->GetDisplayedHealth(), 0.0f);
	TestEqual(TEXT("无有效上限时比例安全归零"), Widget->GetDisplayedPercent(), 0.0f);
	return true;
}

#endif
