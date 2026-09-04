#if WITH_DEV_AUTOMATION_TESTS

#include "City/CityBuildingPieceDefinition.h"
#include "Combustion/BuildCombustionCatalog.h"
#include "Entity/BuildEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "WorldSeed/MillionBuildingRecipe.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCitySurfaceRegistrationTest,
	"ElementSandbox.BuildingCatalog.City.SurfaceMetadataSurvivesRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCitySurfaceRegistrationTest::RunTest(const FString& Parameters)
{
	TSet<FName> SeenSurfaces;
	for (const ECityBuildingArchetype Archetype : GetDefaultCityBuildingArchetypes())
	{
		UCityBuildingRecipe* Recipe = NewObject<UCityBuildingRecipe>();
		if (!TestTrue(TEXT("创建带中性 Surface 的城市配方"), Recipe && Recipe->Initialize(Archetype)))
		{
			continue;
		}
		for (const FCityBuildingPieceRecipe& Piece : Recipe->GetPieces())
		{
			TestFalse(TEXT("每个配方 Part 都保留 SurfaceProfileId"), Piece.SurfaceProfileId.IsNone());
			SeenSurfaces.Add(Piece.SurfaceProfileId);
		}
	}
	TestTrue(TEXT("配方保留 Wall 标签"), SeenSurfaces.Contains(TEXT("Surface.City.Wall")));
	TestTrue(TEXT("配方保留 Stone 标签"), SeenSurfaces.Contains(TEXT("Surface.City.Stone")));
	TestTrue(TEXT("配方保留 Wood 标签"), SeenSurfaces.Contains(TEXT("Surface.City.Wood")));

	for (const FName SurfaceProfileId : GetDefaultCityPieceSurfaceProfileIds())
	{
		UCityBuildingPieceDefinition* Definition = NewObject<UCityBuildingPieceDefinition>();
		if (!TestTrue(TEXT("每个 Surface 可建立独立稳定 Definition"),
			Definition && Definition->Initialize(ECityBuildingPieceKind::SolidBox, SurfaceProfileId)))
		{
			continue;
		}
		TestEqual(TEXT("Mesh Part 保留中性 Surface 标签"),
			Definition->MeshParts[0].SurfaceProfileId, SurfaceProfileId);

		FBuildEntityRegistry Registry;
		const FBuildEntityHandle Entity = Definition->CreateEntity(Registry, FTransform::Identity);
			int32 BurnCustomDataIndex = INDEX_NONE;
			TestTrue(TEXT("燃烧资格只存在于共享 Catalog Definition"),
				TryGetBuildCombustionConfiguration(*Definition, BurnCustomDataIndex)
					&& BurnCustomDataIndex == UCityBuildingPieceDefinition::BurnAmountCustomDataIndex);
	}
	return true;
}

#endif
