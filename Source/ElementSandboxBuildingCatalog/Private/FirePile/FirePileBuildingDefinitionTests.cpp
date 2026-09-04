#if WITH_DEV_AUTOMATION_TESTS

#include "FirePile/FirePileBuildingDefinition.h"

#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Combustion/BuildCombustionCatalog.h"
#include "Entity/BuildEntityRegistry.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFirePileDefinitionConfiguresBuildingTest,
	"ElementSandbox.BuildingCatalog.FirePile.ConfiguresBuilding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFirePileDefinitionConfiguresBuildingTest::RunTest(const FString& Parameters)
{
	UFirePileBuildingDefinition* Definition = NewObject<UFirePileBuildingDefinition>();
	TestNotNull(TEXT("创建 Fire Pile Definition"), Definition);
	if (!Definition)
	{
		return false;
	}

	TestEqual(TEXT("石环、柴堆与火焰占位共十四个 Mesh Part"),
		Definition->MeshParts.Num(), 14);
	for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
	{
		const FBuildMeshPartDefinition& Part = Definition->MeshParts[PartId];
		TestNotNull(TEXT("Fire Pile Part 有 Mesh"), Part.Mesh.Get());
		TestNotNull(TEXT("Fire Pile Part 有明确材质"), Part.MaterialOverride.Get());
		TestTrue(TEXT("Fire Pile 是稳定 Static 表现"),
			Part.PresentationPolicy == EBuildMeshPartPresentationPolicy::Static);
		if (Part.MaterialOverride)
		{
			const TCHAR* ExpectedMaterialPath = PartId < 8
				? TEXT("/Game/Building/Materials/MI_FirePileStone.MI_FirePileStone")
				: PartId < 11
					? TEXT("/Game/Building/Materials/MI_FirePileWood.MI_FirePileWood")
					: TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame");
			TestEqual(
				FString::Printf(TEXT("Fire Pile Part %d 使用正确分类材质"), PartId),
				Part.MaterialOverride->GetPathName(),
				FString(ExpectedMaterialPath));
			TestTrue(
				FString::Printf(TEXT("Fire Pile Part %d 材质支持实例化渲染"), PartId),
				Part.MaterialOverride
					->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes));
		}
	}
	TestEqual(TEXT("Fire Pile 只有一个简单碰撞代理"),
		Definition->CollisionParts.Num(), 1);
	TestTrue(TEXT("Fire Pile 碰撞配置有效"),
		Definition->HasValidCollisionDefinition());

	FBuildEntityRegistry Registry;
	const FBuildEntityHandle Entity = Definition->CreateEntity(
		Registry,
		FTransform(FVector(125.0, -75.0, 0.0)));
	TestTrue(TEXT("Fire Pile Definition 创建有效 Building Entity"),
		Registry.IsAlive(Entity));
	EBuildFixedFireEmitterKind EmitterKind = EBuildFixedFireEmitterKind::MountedTorch;
	TestTrue(TEXT("Fire Pile Definition 由 Catalog 显式登记为固定火源"),
		TryGetBuildFixedFireEmitterKind(Definition->DefinitionId, EmitterKind));
	TestTrue(TEXT("固定火源种类保持 FirePile"),
		EmitterKind == EBuildFixedFireEmitterKind::FirePile);
	return true;
}

#endif
