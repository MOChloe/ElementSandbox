#include "Tree/SettlementTreeMeshFactory.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const FVector4f TrunkColor(0.27f, 0.105f, 0.035f, 1.0f);
	const FVector4f CrownColor(0.10f, 0.48f, 0.075f, 1.0f);

	void AppendTriangle(
		FMeshDescriptionBuilder& Builder,
		const FPolygonGroupID Group,
		const FVector& A,
		const FVector& B,
		const FVector& C,
		const FVector4f& Color)
	{
		const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		const FVector Tangent = (B - A).GetSafeNormal();
		const FVertexID Vertices[] = {
			Builder.AppendVertex(A), Builder.AppendVertex(B), Builder.AppendVertex(C) };
		FVertexInstanceID Instances[3];
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Instances[Index] = Builder.AppendInstance(Vertices[Index]);
			Builder.SetInstanceTangentSpace(Instances[Index], Normal, Tangent, 1.0f);
			Builder.SetInstanceColor(Instances[Index], Color);
			Builder.SetInstanceUV(Instances[Index], FVector2D::ZeroVector);
		}
		Builder.AppendTriangle(Instances[0], Instances[1], Instances[2], Group);
	}

	void AppendBoxTrunk(
		FMeshDescriptionBuilder& Builder,
		const FPolygonGroupID Group,
		const double HalfWidth,
		const double MinZ,
		const double MaxZ)
	{
		const FVector Bottom0(-HalfWidth, -HalfWidth, MinZ);
		const FVector Bottom1(HalfWidth, -HalfWidth, MinZ);
		const FVector Bottom2(HalfWidth, HalfWidth, MinZ);
		const FVector Bottom3(-HalfWidth, HalfWidth, MinZ);
		const FVector Top0(-HalfWidth, -HalfWidth, MaxZ);
		const FVector Top1(HalfWidth, -HalfWidth, MaxZ);
		const FVector Top2(HalfWidth, HalfWidth, MaxZ);
		const FVector Top3(-HalfWidth, HalfWidth, MaxZ);

		// 树干两端分别埋入地面与树冠，不生成端盖。顶盖曾在树冠单面观察角度下
		// 露成一个悬空正方形；开放端既不可见，也避免与树冠封底发生深度穿插。
		AppendTriangle(Builder, Group, Bottom0, Bottom1, Top1, TrunkColor);
		AppendTriangle(Builder, Group, Bottom0, Top1, Top0, TrunkColor);
		AppendTriangle(Builder, Group, Bottom1, Bottom2, Top2, TrunkColor);
		AppendTriangle(Builder, Group, Bottom1, Top2, Top1, TrunkColor);
		AppendTriangle(Builder, Group, Bottom2, Bottom3, Top3, TrunkColor);
		AppendTriangle(Builder, Group, Bottom2, Top3, Top2, TrunkColor);
		AppendTriangle(Builder, Group, Bottom3, Bottom0, Top0, TrunkColor);
		AppendTriangle(Builder, Group, Bottom3, Top0, Top3, TrunkColor);
	}

	void AppendCrownCone(
		FMeshDescriptionBuilder& Builder,
		const FPolygonGroupID Group,
		const int32 Sides,
		const double Radius,
		const double BaseZ,
		const double TopZ)
	{
		const FVector Apex(0.0, 0.0, TopZ);
		const FVector Center(0.0, 0.0, BaseZ);
		for (int32 Side = 0; Side < Sides; ++Side)
		{
			const double Angle0 = UE_TWO_PI * Side / Sides;
			const double Angle1 = UE_TWO_PI * (Side + 1) / Sides;
			const FVector Edge0(Radius * FMath::Cos(Angle0), Radius * FMath::Sin(Angle0), BaseZ);
			const FVector Edge1(Radius * FMath::Cos(Angle1), Radius * FMath::Sin(Angle1), BaseZ);
			AppendTriangle(Builder, Group, Edge0, Edge1, Apex, CrownColor);
			AppendTriangle(Builder, Group, Center, Edge1, Edge0, CrownColor);
		}
	}

	void BuildLOD(FMeshDescription& Description, const int32 LODIndex)
	{
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		FMeshDescriptionBuilder Builder;
		Builder.SetMeshDescription(&Description);
		Builder.SetNumUVLayers(1);
		const FPolygonGroupID Group = Builder.AppendPolygonGroup(TEXT("SettlementTree"));
		const int32 Sides = LODIndex == 0 ? 8 : (LODIndex == 1 ? 6 : 4);
		// 正式种子会把整树放大约三倍；树干横截面在基础网格中反向收窄，
		// 避免总高度正确但近处变成直径 2.7m 的八棱桶。
		AppendBoxTrunk(Builder, Group, 15.0, 0.0, 235.0);
		// 所有 LOD 都保持同一个清晰轮廓：两个互相重叠且各自封底的立体圆锥。
		// 仅降低圆周边数，不在切换 LOD 时改变树冠层数。
		AppendCrownCone(Builder, Group, Sides, 220.0, 190.0, 485.0);
		AppendCrownCone(Builder, Group, Sides, 175.0, 330.0, 620.0);
	}
}

UStaticMesh* FSettlementTreeMeshFactory::Create(UObject& Outer)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(&Outer, TEXT("SettlementTreeRuntimeMesh"), RF_Transient);
	if (!Mesh)
	{
		return nullptr;
	}
	UMaterialInterface* VertexColorMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WorldObjects/Trees/M_SettlementTree.M_SettlementTree"));
	if (!VertexColorMaterial)
	{
		VertexColorMaterial = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/VertexColorMaterial.VertexColorMaterial"));
	}
	if (!VertexColorMaterial)
	{
		VertexColorMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(VertexColorMaterial, TEXT("SettlementTree")));

	TArray<FMeshDescription> Descriptions;
	Descriptions.SetNum(3);
	TArray<const FMeshDescription*> DescriptionViews;
	for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
	{
		BuildLOD(Descriptions[LODIndex], LODIndex);
		DescriptionViews.Add(&Descriptions[LODIndex]);
	}
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	if (!Mesh->BuildFromMeshDescriptions(DescriptionViews, Params) || !Mesh->GetRenderData())
	{
		return nullptr;
	}
	Mesh->GetRenderData()->ScreenSize[0].Default = 1.0f;
	Mesh->GetRenderData()->ScreenSize[1].Default = 0.10f;
	Mesh->GetRenderData()->ScreenSize[2].Default = 0.025f;
	Mesh->CreateBodySetup();
	if (UBodySetup* BodySetup = Mesh->GetBodySetup())
	{
		BodySetup->AggGeom.EmptyElements();
		FKSphylElem& Trunk = BodySetup->AggGeom.SphylElems.AddDefaulted_GetRef();
		Trunk.Center = FVector(0.0, 0.0, 130.0);
		Trunk.Radius = 15.0f;
		Trunk.Length = 230.0f;
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	}
	return Mesh;
}
