// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/ElementSandboxDemoFallbackGround.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AElementSandboxDemoFallbackGround::AElementSandboxDemoFallbackGround()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(1.0f);
	Tags.Add(TEXT("ElementSandbox.DemoFallbackGround"));

	UStaticMeshComponent* Mesh = GetStaticMeshComponent();
	check(Mesh);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> GroundMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GroundMaterial(
		TEXT("/Game/Building/Materials/M_DemoFallbackGround.M_DemoFallbackGround"));
	Mesh->SetStaticMesh(GroundMesh.Object);
	Mesh->SetMaterial(0, GroundMaterial.Object);
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetReceivesDecals(false);
	Mesh->SetCanEverAffectNavigation(false);
	SetActorEnableCollision(true);
}
