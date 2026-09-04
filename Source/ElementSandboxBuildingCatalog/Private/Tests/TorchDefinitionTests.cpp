#if WITH_DEV_AUTOMATION_TESTS

#include "Combustion/BuildCombustionCatalog.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Torch/TorchDefinition.h"
#include "Torch/TorchFixtureBuildingDefinition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTorchContextualFormsTest,
	"ElementSandbox.BuildingCatalog.Torch.ContextualForms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTorchContextualFormsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const UTorchDefinition* Torch = GetDefault<UTorchDefinition>();
	FString Error;
	TestTrue(TEXT("默认火把内容配置合法"), Torch && Torch->IsValid(&Error));
	if (!Torch)
	{
		return false;
	}

	FName DefinitionId;
	TestTrue(TEXT("当前已实现 MountedBuilding 形态"),
		Torch->TryResolveForm(ETorchForm::MountedBuilding, DefinitionId));
	TestEqual(TEXT("MountedBuilding 使用稳定 DefinitionId"),
		DefinitionId, GetMountedTorchBuildingDefinitionId());
	TestFalse(TEXT("尚未实现的手持形态不会伪装成 Building"),
		Torch->TryResolveForm(ETorchForm::Equipped, DefinitionId));

	const UTorchFixtureBuildingDefinition* Fixture = GetDefault<UTorchFixtureBuildingDefinition>();
	int32 BurnCustomDataIndex = INDEX_NONE;
	TestTrue(TEXT("挂墙火炬同时显式声明为普通可燃 Building"),
		Fixture && TryGetBuildCombustionConfiguration(*Fixture, BurnCustomDataIndex)
			&& BurnCustomDataIndex == 0);
	if (Fixture && Fixture->MeshParts.Num() == 2)
	{
		TestEqual(TEXT("火炬木杆预留 BurnAmount Custom Data"),
			Fixture->MeshParts[0].CustomDataFloatCount, 1);
		TestTrue(TEXT("火炬木杆使用统一可烧黑材质"),
			Fixture->MeshParts[0].MaterialOverride
				&& Fixture->MeshParts[0].MaterialOverride->GetPathName()
					== TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable"));
		TestEqual(TEXT("火炬火焰仍是纯表现，不读取 BurnAmount"),
			Fixture->MeshParts[1].CustomDataFloatCount, 0);
	}
	TestEqual(TEXT("火炬只为木杆声明一份近场碰撞"),
		Fixture ? Fixture->CollisionParts.Num() : 0, 1);
	if (Fixture && Fixture->CollisionParts.Num() == 1)
	{
		TestEqual(TEXT("火炬碰撞跟随木杆 Mesh Part"),
			Fixture->CollisionParts[0].DrivenMeshPartId, 0);
	}

	UTorchDefinition* Invalid = NewObject<UTorchDefinition>();
	const FTorchFormBinding Duplicate = Invalid->Forms[0];
	Invalid->Forms.Add(Duplicate);
	TestFalse(TEXT("重复 Form 配置被严格拒绝"), Invalid->IsValid(&Error));
	return true;
}

#endif
