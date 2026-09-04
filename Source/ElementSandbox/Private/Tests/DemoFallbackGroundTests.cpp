#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Game/ElementSandboxDemoFallbackGround.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDemoFallbackGroundCollisionTest,
	"ElementSandbox.Gameplay.Map.DemoFallbackGroundCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDemoFallbackGroundCollisionTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues InitializationValues;
	InitializationValues
		.CreatePhysicsScene(true)
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(false)
		.CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		TEXT("DemoFallbackGroundCollision"),
		nullptr,
		true,
		ERHIFeatureLevel::Num,
		&InitializationValues,
		true);
	if (!TestNotNull(TEXT("创建兜底地面测试世界"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
	World->SetPlayInEditorInitialNetMode(NM_Standalone);
	World->InitWorld(InitializationValues);
	World->UpdateWorldComponents(true, false);

	const FTransform GroundTransform(
		FRotator::ZeroRotator,
		FVector(0.0, 0.0, -5.0),
		FVector(20.0, 20.0, 0.1));
	AElementSandboxDemoFallbackGround* Ground =
		World->SpawnActor<AElementSandboxDemoFallbackGround>(
			AElementSandboxDemoFallbackGround::StaticClass(), GroundTransform);
	UStaticMeshComponent* Mesh = Ground ? Ground->GetStaticMeshComponent() : nullptr;
	const bool bGroundCreated = TestTrue(TEXT("专用兜底地面带有有效 StaticMesh"),
		Ground && Mesh && IsValid(Mesh->GetStaticMesh()));
	if (bGroundCreated)
	{
		TestEqual(TEXT("兜底地面保持 Static Mobility"),
			Mesh->Mobility, EComponentMobility::Static);
		TestEqual(TEXT("兜底地面阻挡 Pawn"),
			Mesh->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
		TestTrue(TEXT("兜底地面在客户端由 Actor 复制创建"),
			Ground->GetIsReplicated() && Ground->bAlwaysRelevant);
		UMaterialInterface* GroundMaterial = Mesh->GetMaterial(0);
			TestNotNull(TEXT("兜底地面使用专用无光照白色材质"), GroundMaterial);
			if (GroundMaterial)
			{
				TestEqual(TEXT("兜底地面材质资源固定"), GroundMaterial->GetPathName(),
					FString(TEXT("/Game/Building/Materials/M_DemoFallbackGround.M_DemoFallbackGround")));
				TestTrue(TEXT("兜底地面材质不受场景天光染色"),
					GroundMaterial->GetShadingModels().HasShadingModel(MSM_Unlit));
			}

		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(
			Hit,
			FVector(0.0, 0.0, 200.0),
			FVector(0.0, 0.0, -200.0),
			ECC_Pawn);
		TestTrue(TEXT("Pawn Channel 射线命中兜底地面"),
			bHit && Hit.GetActor() == Ground);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bGroundCreated;
}

#endif
