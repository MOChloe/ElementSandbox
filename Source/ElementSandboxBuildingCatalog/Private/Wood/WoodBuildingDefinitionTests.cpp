#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Combustion/BuildCombustionCatalog.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Materials/MaterialInterface.h"
#include "Wood/WoodBuildingDefinition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoodBuildingDefinitionsFireStateTest,
	"ElementSandbox.BuildingCatalog.Wood.FireStateWithoutBurnTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoodBuildingDefinitionsFireStateTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FName DefinitionId;
		FVector Size;
	};
	const FCase Cases[] = {
		{TEXT("WoodWall"), FVector(20.0, 400.0, 300.0)},
		{TEXT("WoodFloor"), FVector(400.0, 400.0, 20.0)},
		{TEXT("WoodPillar"), FVector(20.0, 20.0, 300.0)},
	};

	for (const FCase& TestCase : Cases)
	{
		UWoodBuildingDefinition* Definition = NewObject<UWoodBuildingDefinition>();
		const FString Prefix = TestCase.DefinitionId.ToString();
		if (!TestTrue(*FString::Printf(TEXT("%s Definition 初始化"), *Prefix),
			Definition && Definition->Initialize(TestCase.DefinitionId, TestCase.Size)))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s 配置统一斧头破坏产物"), *Prefix),
			Definition->Destruction.IsEnabled() && Definition->Destruction.IsValid());
		TestEqual(*FString::Printf(TEXT("%s 只有一份实例 Mesh"), *Prefix),
			Definition->MeshParts.Num(), 1);
			if (Definition->MeshParts.Num() == 1)
			{
				const FBuildMeshPartDefinition& MeshPart = Definition->MeshParts[0];
				TestEqual(*FString::Printf(TEXT("%s 声明一个 BurnAmount"), *Prefix),
				MeshPart.CustomDataFloatCount, 1);
			TestTrue(*FString::Printf(TEXT("%s 使用可烧黑材质"), *Prefix),
				MeshPart.MaterialOverride
					&& MeshPart.MaterialOverride->GetPathName()
						== TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
			TestTrue(*FString::Printf(TEXT("%s 可烧黑材质支持 HISM"), *Prefix),
				MeshPart.MaterialOverride
					&& MeshPart.MaterialOverride->CheckMaterialUsage_Concurrent(
						MATUSAGE_InstancedStaticMeshes));
		}

		FBuildEntityRegistry Registry;
		const FBuildEntityHandle Entity =
			Definition->CreateEntity(Registry, FTransform::Identity);
		const FBuildRenderCustomDataFragment* BurnData =
			Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity);
		const FBuildDamageFragment* Damage =
			Registry.FindFragment<FBuildDamageFragment>(Entity);
		int32 BurnCustomDataIndex = INDEX_NONE;
		TestTrue(*FString::Printf(TEXT("%s 由 Catalog 显式声明燃烧资格"), *Prefix),
			TryGetBuildCombustionConfiguration(*Definition, BurnCustomDataIndex)
				&& BurnCustomDataIndex == 0);
		TestNull(*FString::Printf(TEXT("%s 稳定 Cold 不分配 Entity CustomData"), *Prefix),
			BurnData);
		TestTrue(*FString::Printf(TEXT("%s Entity 创建成功"), *Prefix), Registry.IsAlive(Entity));
		TestNull(*FString::Printf(TEXT("%s 未受击时不分配伤害状态"), *Prefix), Damage);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoodBuildingColdPopulationStaysSparseTest,
	"ElementSandbox.BuildingCatalog.Wood.ColdPopulationStaysSparse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoodBuildingColdPopulationStaysSparseTest::RunTest(const FString& Parameters)
{
	constexpr int32 Population = 10000;
	UWoodBuildingDefinition* Definition = NewObject<UWoodBuildingDefinition>();
	if (!TestTrue(TEXT("稀疏规模测试 Definition 初始化"),
		Definition && Definition->Initialize(TEXT("WoodSparsePopulation"), FVector(400.0, 20.0, 300.0))))
	{
		return false;
	}

	FBuildEntityRegistry Registry;
	for (int32 Index = 0; Index < Population; ++Index)
	{
		if (!Registry.IsAlive(Definition->CreateEntity(
			Registry, FTransform(FVector(Index * 10.0, 0.0, 0.0)))))
		{
			AddError(FString::Printf(TEXT("第 %d 个 Cold Entity 创建失败。"), Index));
			return false;
		}
	}

	TestEqual(TEXT("规模测试创建全部 Cold Entity"), Registry.GetEntityCount(), Population);
	TestEqual(TEXT("Cold Population 的 Burn CustomData 池为空"),
		Registry.GetFragmentCount<FBuildRenderCustomDataFragment>(), 0);
	TestEqual(TEXT("未受击 Population 的伤害状态池为空"),
		Registry.GetFragmentCount<FBuildDamageFragment>(), 0);
	return true;
}

#endif
