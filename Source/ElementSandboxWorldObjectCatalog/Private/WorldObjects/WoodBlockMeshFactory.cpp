#include "WorldObjects/WoodBlockMeshFactory.h"

#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"

namespace
{
	void AppendTriangle(
		FMeshDescriptionBuilder& Builder,
		const FPolygonGroupID Group,
		const FVector& A,
		const FVector& B,
		const FVector& C,
		const FVector2D& UvA,
		const FVector2D& UvB,
		const FVector2D& UvC)
	{
		const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		const FVector Tangent = (B - A).GetSafeNormal();
		const FVector Points[] = {A, B, C};
		const FVector2D Uvs[] = {UvA, UvB, UvC};
		FVertexInstanceID Instances[3];
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FVertexID Vertex = Builder.AppendVertex(Points[Index]);
			Instances[Index] = Builder.AppendInstance(Vertex);
			Builder.SetInstanceTangentSpace(Instances[Index], Normal, Tangent, 1.0f);
			Builder.SetInstanceUV(Instances[Index], Uvs[Index]);
		}
		// UE MeshDescription 使用左手坐标系的绕序约定。上面的右手 CrossProduct
		// 仍用于写入朝外法线；提交时反转 B/C，使几何正面和该法线同向。
		Builder.AppendTriangle(Instances[0], Instances[2], Instances[1], Group);
	}

	void AppendQuad(
		FMeshDescriptionBuilder& Builder,
		const FPolygonGroupID Group,
		const FVector& A,
		const FVector& B,
		const FVector& C,
		const FVector& D)
	{
		AppendTriangle(Builder, Group, A, B, C, FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, 1.0));
		AppendTriangle(Builder, Group, A, C, D, FVector2D(0.0, 0.0), FVector2D(1.0, 1.0), FVector2D(0.0, 1.0));
	}
}

UStaticMesh* FWoodBlockMeshFactory::Create(
	UObject& Outer,
	const FName ObjectName,
	const EObjectFlags Flags)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(&Outer, ObjectName, Flags);
	if (!Mesh)
	{
		return nullptr;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(
		nullptr,
		TEXT("/Game/Building/Materials/MI_FirePileWood.MI_FirePileWood"));
	if (!Material)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Wood")));

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	FMeshDescriptionBuilder Builder;
	Builder.SetMeshDescription(&Description);
	Builder.SetNumUVLayers(1);
	const FPolygonGroupID Group = Builder.AppendPolygonGroup(TEXT("Wood"));
	const FVector Min(-70.0, -24.0, -20.0);
	const FVector Max(70.0, 24.0, 20.0);
	const FVector V000(Min.X, Min.Y, Min.Z);
	const FVector V001(Min.X, Min.Y, Max.Z);
	const FVector V010(Min.X, Max.Y, Min.Z);
	const FVector V011(Min.X, Max.Y, Max.Z);
	const FVector V100(Max.X, Min.Y, Min.Z);
	const FVector V101(Max.X, Min.Y, Max.Z);
	const FVector V110(Max.X, Max.Y, Min.Z);
	const FVector V111(Max.X, Max.Y, Max.Z);
	AppendQuad(Builder, Group, V000, V010, V110, V100);
	AppendQuad(Builder, Group, V001, V101, V111, V011);
	AppendQuad(Builder, Group, V000, V100, V101, V001);
	AppendQuad(Builder, Group, V010, V011, V111, V110);
	AppendQuad(Builder, Group, V000, V001, V011, V010);
	AppendQuad(Builder, Group, V100, V110, V111, V101);

	const TArray<const FMeshDescription*> Descriptions{&Description};
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = true;
	Params.bFastBuild = false;
	Params.bBuildSimpleCollision = false;
	if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params) || !Mesh->GetRenderData())
	{
		return nullptr;
	}
	Mesh->CreateBodySetup();
	if (UBodySetup* BodySetup = Mesh->GetBodySetup())
	{
		BodySetup->AggGeom.EmptyElements();
		FKBoxElem& Box = BodySetup->AggGeom.BoxElems.AddDefaulted_GetRef();
		Box.Center = FVector::ZeroVector;
		Box.X = 70.0f;
		Box.Y = 24.0f;
		Box.Z = 20.0f;
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	}
	return Mesh;
}
