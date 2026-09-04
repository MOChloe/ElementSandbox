#if WITH_DEV_AUTOMATION_TESTS

#include "Fire/ElementFireRuleSet.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireRuleSetFreezeTest,
	"ElementSandbox.Element.Fire.RuleSet.StrictFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireRuleSetFreezeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const UElementFireRuleSet* Rules = GetDefault<UElementFireRuleSet>();
	FFireRuleSnapshot Snapshot;
	FString Error;
	TestTrue(TEXT("默认首轮规则可冻结"), Rules->Freeze(Snapshot, Error));
	TestEqual(TEXT("耐燃度调参已提升规则版本"), Snapshot.Revision, 2ull);
	TestEqual(TEXT("木结构点燃 Dose"), Snapshot.Structure.IgnitionDose, 6.0);
	TestEqual(TEXT("木结构传播强度保持逐层点燃节奏"),
		Snapshot.Structure.EmittedFireIntensity, 1.0);
	TestEqual(TEXT("木结构传播范围只覆盖贴邻构件"),
		Snapshot.Structure.EmissionRangeCentimeters, 80.0);
	TestEqual(TEXT("Fireball 强度匹配木结构耐燃度"), Snapshot.Fireball.Intensity, 3.0);
	TestEqual(TEXT("Fireball 不一次覆盖整片建筑"), Snapshot.Fireball.RangeCentimeters, 50.0);
	TestEqual(TEXT("Fireball 持续灼烧而非瞬间点燃"), Snapshot.FireballLifetimeMilliseconds, 3000ll);
	TestEqual(TEXT("FirePile 是 CharacterOnly"), Snapshot.FirePile.Policy,
		EFirePropagationPolicy::CharacterOnly);
	TestEqual(TEXT("挂墙火把固定排除 Building"), Snapshot.MountedTorch.Policy,
		EFirePropagationPolicy::CharacterOnly);
	TestEqual(TEXT("挂墙火把强度"), Snapshot.MountedTorch.Intensity, 1.0);
	TestEqual(TEXT("挂墙火把范围"), Snapshot.MountedTorch.RangeCentimeters, 80.0);
	TestTrue(TEXT("挂墙火把火源中心与可见火焰中心一致"),
		Snapshot.MountedTorchSphereCenter.Equals(FVector(-24.0, 0.0, 92.0)));
	TestEqual(TEXT("挂墙火把火源球半径"), Snapshot.MountedTorchSphereRadius, 18.0);

	const UElementFireRuleSet* AssetRules = LoadObject<UElementFireRuleSet>(
		nullptr, UElementFireRuleSet::GetDefaultAssetPath());
	TestNotNull(TEXT("正式火焰规则资产可加载"), AssetRules);
	if (AssetRules)
	{
		FFireRuleSnapshot AssetSnapshot;
		FString AssetError;
		TestTrue(TEXT("正式火焰规则资产可冻结"), AssetRules->Freeze(AssetSnapshot, AssetError));
		TestEqual(TEXT("正式规则资产使用耐燃度调参版本"), AssetSnapshot.Revision, 2ull);
		TestEqual(TEXT("正式规则资产使用木结构慢传播强度"),
			AssetSnapshot.Structure.EmittedFireIntensity, 1.0);
		TestEqual(TEXT("正式规则资产使用木结构近邻传播范围"),
			AssetSnapshot.Structure.EmissionRangeCentimeters, 80.0);
		TestEqual(TEXT("正式规则资产使用收紧后的 Fireball 强度"),
			AssetSnapshot.Fireball.Intensity, 3.0);
		TestEqual(TEXT("正式规则资产使用收紧后的 Fireball 范围"),
			AssetSnapshot.Fireball.RangeCentimeters, 50.0);
		TestEqual(TEXT("正式规则资产使用延长后的 Fireball 时长"),
			AssetSnapshot.FireballLifetimeMilliseconds, 3000ll);
		TestTrue(TEXT("正式规则资产的挂墙火源中心与可见火焰中心一致"),
			AssetSnapshot.MountedTorchSphereCenter.Equals(FVector(-24.0, 0.0, 92.0)));
	}

	UElementFireRuleSet* Invalid = NewObject<UElementFireRuleSet>();
	Invalid->StackTwoExitIntensity = Invalid->StackTwoEnterIntensity;
	TestFalse(TEXT("相等的迟滞阈值被拒绝"), Invalid->Freeze(Snapshot, Error));
	return true;
}

#endif
