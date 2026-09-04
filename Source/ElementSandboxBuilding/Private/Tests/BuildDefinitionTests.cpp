#include "Tests/BuildEntityTestTypes.h"

#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildTransformFragment.h"

UBuildTestDefinition::UBuildTestDefinition()
{
	DefinitionId = TEXT("Building.Test.Generic");
}

bool UBuildTestDefinition::ConfigureEntity(
	FBuildEntityRegistry& Registry,
	const FBuildEntityHandle Entity) const
{
	if (!bAllowConfiguration)
	{
		return false;
	}

	FBuildTestValueFragment Fragment;
	Fragment.Value = InitialValue;
	Fragment.Label = TEXT("Definition");
	return Registry.AddFragment(Entity, Fragment);
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDefinitionCreatesEntityTest,
	"ElementSandbox.Building.Definition.CreatesConfiguredEntity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDefinitionCreatesEntityTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>();
	Definition->InitialValue = 37;
	const FTransform InitialWorldTransform(
		FRotator(0.0, 45.0, 0.0),
		FVector(100.0, 200.0, 300.0),
		FVector(1.0, 2.0, 1.0));

	const FBuildEntityHandle Entity = Definition->CreateEntity(
		Registry,
		InitialWorldTransform);
	TestTrue(TEXT("Definition 创建有效 Entity"), Registry.IsAlive(Entity));
	TestEqual(TEXT("只创建一个运行时 Entity"), Registry.GetEntityCount(), 1);

	const FBuildTransformFragment* TransformFragment =
		Registry.FindFragment<FBuildTransformFragment>(Entity);
	TestNotNull(TEXT("基类原子写入通用 Transform Fragment"), TransformFragment);
	if (TransformFragment)
	{
		TestTrue(TEXT("Transform Fragment 保存完整初始世界位姿"),
			TransformFragment->WorldTransform.Equals(InitialWorldTransform));
	}

	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	TestNotNull(TEXT("基类原子写入通用 Definition Fragment"), DefinitionFragment);
	if (DefinitionFragment)
	{
		TestTrue(TEXT("Definition Fragment 直接强持有创建它的 Definition"),
			DefinitionFragment->Definition.Get() == Definition);
	}

	const FBuildTestValueFragment* Fragment =
		Registry.FindFragment<FBuildTestValueFragment>(Entity);
	TestNotNull(TEXT("派生 Definition 写入自己的初始 Fragment"), Fragment);
	if (Fragment)
	{
		TestEqual(TEXT("初始配置进入 Fragment Pool"), Fragment->Value, 37);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDefinitionRollsBackFailureTest,
	"ElementSandbox.Building.Definition.RollbackOnConfigurationFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDefinitionRollsBackFailureTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>();
	Definition->bAllowConfiguration = false;

	const FBuildEntityHandle Entity = Definition->CreateEntity(
		Registry,
		FTransform::Identity);
	TestFalse(TEXT("配置失败不返回可用 Handle"), Entity.IsSet());
	TestEqual(TEXT("配置失败不会留下半初始化 Entity"), Registry.GetEntityCount(), 0);
	TestEqual(TEXT("配置失败不会留下 Fragment"),
		Registry.GetFragmentCount<FBuildTestValueFragment>(), 0);
	TestEqual(TEXT("配置失败会回滚通用 Transform Fragment"),
		Registry.GetFragmentCount<FBuildTransformFragment>(), 0);
	TestEqual(TEXT("配置失败会回滚通用 Definition Fragment"),
		Registry.GetFragmentCount<FBuildDefinitionFragment>(), 0);
	return true;
}

#endif
