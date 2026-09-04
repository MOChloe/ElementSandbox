#if WITH_DEV_AUTOMATION_TESTS

#include "WorldObjects/WoodBlockMeshFactory.h"

#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoodBlockMeshContractTest,
	"ElementSandbox.WorldObjects.WoodBlock.MeshContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoodBlockMeshContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UStaticMesh* Mesh = FWoodBlockMeshFactory::Create(*GetTransientPackage());
	if (!TestNotNull(TEXT("可生成固定木块长方体"), Mesh)
		|| !TestNotNull(TEXT("木块包含 RenderData"), Mesh ? Mesh->GetRenderData() : nullptr)
		|| Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		return false;
	}

	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	TestEqual(TEXT("封闭长方体固定为六面十二个三角形"),
		static_cast<int32>(LOD.GetNumTriangles()), 12);
	TestTrue(TEXT("演示木块尺寸固定为 140x48x40cm"),
		Mesh->GetBounds().BoxExtent.Equals(FVector(70.0, 24.0, 20.0), 0.01));

	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	bool bAllGeometricFacesPointOutward = Indices.Num() == 36;
	for (int32 Index = 0; bAllGeometricFacesPointOutward && Index < Indices.Num(); Index += 3)
	{
		const FVector3f A = Positions.VertexPosition(Indices[Index]);
		const FVector3f B = Positions.VertexPosition(Indices[Index + 1]);
		const FVector3f C = Positions.VertexPosition(Indices[Index + 2]);
		const FVector3f TriangleCenter = (A + B + C) / 3.0f;
		// MeshDescription 在 UE 左手坐标系中用反向 Cross 求几何面法线。
		const FVector3f UnrealFaceNormal = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
		bAllGeometricFacesPointOutward =
			FVector3f::DotProduct(UnrealFaceNormal, TriangleCenter) > 0.99f;
	}
	TestTrue(TEXT("六个几何外表面均朝外，不会显示成内翻空壳"),
		bAllGeometricFacesPointOutward);

	const UBodySetup* BodySetup = Mesh->GetBodySetup();
	TestTrue(TEXT("木块只使用一个同尺寸 Simple Box Collision"),
		BodySetup && BodySetup->AggGeom.BoxElems.Num() == 1
			&& BodySetup->AggGeom.SphereElems.IsEmpty()
			&& BodySetup->AggGeom.SphylElems.IsEmpty()
			&& BodySetup->AggGeom.ConvexElems.IsEmpty()
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].X, 70.0f)
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].Y, 24.0f)
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].Z, 20.0f));
	return true;
}

#endif
