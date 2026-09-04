#if WITH_DEV_AUTOMATION_TESTS

#include "Audit/BuildRegistrationAudit.h"
#include "City/CityBuildingPieceDefinition.h"
#include "WorldSeed/MillionBuildingRecipe.h"

#include "BuildingCatalogWorldSubsystem.h"
#include "BuildingWorldSubsystem.h"
#include "Collision/BuildCollisionTypes.h"
#include "Combustion/BuildCombustionCatalog.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Door/DoorBuildingDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Torch/TorchDefinition.h"
#include "Torch/TorchFixtureBuildingDefinition.h"

namespace
{
UCityBuildingPieceDefinition* FindPrimitiveDefinition(
	const ECityBuildingPieceKind Kind,
	const FName SurfaceProfileId,
	const TArray<TObjectPtr<UCityBuildingPieceDefinition>>& Definitions)
{
	for (UCityBuildingPieceDefinition* Definition : Definitions)
	{
		if (Definition
			&& Definition->GetPieceKind() == Kind
			&& Definition->GetSurfaceProfileId() == SurfaceProfileId)
		{
			return Definition;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCityBuildingRecipeConfigurationTest,
	"ElementSandbox.BuildingCatalog.City.RecipeExpandsToIndependentEntities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCityBuildingRecipeConfigurationTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<ECityBuildingArchetype> Archetypes = GetDefaultCityBuildingArchetypes();
	TestEqual(TEXT("聚落目录提供十二种完整结构配方"), Archetypes.Num(), 12);

	TArray<TObjectPtr<UCityBuildingPieceDefinition>> PrimitiveDefinitions;
	TSet<FName> PrimitiveDefinitionIds;
	for (const ECityBuildingPieceKind Kind : GetDefaultCityPrimitivePieceKinds())
	{
		for (const FName SurfaceProfileId : GetDefaultCityPieceSurfaceProfileIds())
		{
			UCityBuildingPieceDefinition* Definition = NewObject<UCityBuildingPieceDefinition>();
			TestNotNull(TEXT("创建共享城市 primitive Definition"), Definition);
			if (!Definition || !Definition->Initialize(Kind, SurfaceProfileId))
			{
				AddError(TEXT("共享城市 primitive Definition 初始化失败。"));
				continue;
			}
			PrimitiveDefinitions.Add(Definition);
			TestFalse(TEXT("primitive DefinitionId 非空"), Definition->DefinitionId.IsNone());
			TestFalse(TEXT("Kind+Surface primitive DefinitionId 不重复"),
				PrimitiveDefinitionIds.Contains(Definition->DefinitionId));
			PrimitiveDefinitionIds.Add(Definition->DefinitionId);
			TestEqual(TEXT("每种 primitive Definition 只有一个 Mesh Part"), Definition->MeshParts.Num(), 1);
			TestTrue(TEXT("primitive 碰撞配置有效"), Definition->HasValidCollisionDefinition());
			TestTrue(TEXT("primitive 三角面估算有效"), Definition->GetEstimatedTriangleCountPerInstance() > 0);
			if (Definition->MeshParts.Num() == 1)
			{
				const FBuildMeshPartDefinition& MeshPart = Definition->MeshParts[0];
				TestNotNull(TEXT("primitive 有共享 Mesh"), MeshPart.Mesh.Get());
				TestNotNull(TEXT("primitive 有统一可燃烧材质"), MeshPart.MaterialOverride.Get());
				TestEqual(TEXT("primitive 保留中性 Surface 标签"), MeshPart.SurfaceProfileId, SurfaceProfileId);
				TestTrue(TEXT("primitive 使用跨 Entity 合批的 Static HISM 策略"),
					MeshPart.PresentationPolicy == EBuildMeshPartPresentationPolicy::Static);
				TestEqual(TEXT("primitive 材质只读取 BurnAmount"), MeshPart.CustomDataFloatCount,
					UCityBuildingPieceDefinition::CustomDataFloatCount);
			}
			const bool bSolid = Kind == ECityBuildingPieceKind::SolidBox
				|| Kind == ECityBuildingPieceKind::SolidSphere;
			TestEqual(TEXT("只有 Solid primitive 创建一个碰撞代理"),
				Definition->CollisionParts.Num(), bSolid ? 1 : 0);
			if (bSolid && Definition->CollisionParts.Num() == 1)
			{
				const FBuildCollisionPartDefinition& Collision = Definition->CollisionParts[0];
				TestEqual(TEXT("碰撞由唯一 Mesh Part 驱动"), Collision.DrivenMeshPartId, 0);
				TestTrue(TEXT("碰撞不再携带整栋偏移"), Collision.LocalTransform.Equals(FTransform::Identity));
				TestTrue(TEXT("城市结构部件碰撞保持 Static"), Collision.Mobility == EBuildCollisionMobility::Static);
			}
		}
	}
	TestEqual(TEXT("Catalog 为四种几何和三种 Surface 建立十二种 Definition"),
		PrimitiveDefinitions.Num(), 12);

	UDoorBuildingDefinition* DoorDefinition = NewObject<UDoorBuildingDefinition>();
	TestTrue(
		TEXT("原始七部件门可配置为结构配方部件"), DoorDefinition && DoorDefinition->InitializeAsSettlementCompanion());

	TSet<FName> RecipeIds;
	TSet<FIntVector> RoundedSilhouettes;
	int32 TotalPieceEntityCount = 0;
	int32 TotalDoorEntityCount = 0;
	for (const ECityBuildingArchetype Archetype : Archetypes)
	{
		UCityBuildingRecipe* Recipe = NewObject<UCityBuildingRecipe>();
		TestNotNull(TEXT("创建完整结构配方"), Recipe);
		if (!Recipe || !Recipe->Initialize(Archetype))
		{
			AddError(TEXT("完整结构配方初始化失败。"));
			continue;
		}

		TestFalse(TEXT("结构配方有稳定 RecipeId"), Recipe->GetRecipeId().IsNone());
		TestFalse(TEXT("RecipeId 在聚落目录内不重复"), RecipeIds.Contains(Recipe->GetRecipeId()));
		RecipeIds.Add(Recipe->GetRecipeId());
			TestFalse(TEXT("完整结构配方有中文显示名"), Recipe->GetDisplayName().IsEmpty());
			TestTrue(TEXT("结构配方会展开为多个独立 Building Entity"), Recipe->GetPieceEntityCount() >= 10);
			FString AssemblyError;
			const bool bAssemblyValid = Recipe->ValidateAssemblyGeometry(&AssemblyError);
			TestTrue(
				FString::Printf(TEXT("%s 的所有部件接地且火把拥有真实承载点：%s"),
					*Recipe->GetRecipeId().ToString(), *AssemblyError),
				bAssemblyValid);
			TestEqual(TEXT("完整结构有两个独立挂墙火把插槽"), Recipe->GetMountedTorchEntityCount(), 2);
		for (const FTransform& TorchTransform : Recipe->GetMountedTorchLocalTransforms())
		{
			TestFalse(TEXT("挂墙火把插槽 Transform 不含 NaN"), TorchTransform.ContainsNaN());
			TestTrue(TEXT("挂墙火把插槽保持单位比例"),
				TorchTransform.GetScale3D().Equals(FVector::OneVector));
		}
		TestTrue(TEXT("单结构部件数留在 256 个确定性槽位内"), Recipe->GetPieceEntityCount() <= 256);
		TestTrue(
			TEXT("结构 Recipe UObject 本身不是 UBuildingDefinition"), !Recipe->IsA(UBuildingDefinition::StaticClass()));

		FBuildEntityRegistry Registry;
		int32 CreatedDoorCount = 0;
		int32 CreatedPrimitiveCount = 0;
		for (const FCityBuildingPieceRecipe& Piece : Recipe->GetPieces())
		{
			TestTrue(TEXT("每个配方部件 Transform 有效"), Piece.IsValid());
				UBuildingDefinition* Definition = Piece.IsDoor()
					? static_cast<UBuildingDefinition*>(DoorDefinition)
					: FindPrimitiveDefinition(Piece.Kind, Piece.SurfaceProfileId, PrimitiveDefinitions);
			if (!TestNotNull(TEXT("每个配方部件都能解析共享 Definition"), Definition))
			{
				continue;
			}
			const FBuildEntityHandle Entity = Definition->CreateEntity(Registry, Piece.LocalTransform);
			TestTrue(TEXT("配方每一项都创建独立有效 Building Entity"), Registry.IsAlive(Entity));
			if (Piece.IsDoor())
			{
				++CreatedDoorCount;
				TestTrue(TEXT("配方门保留原始单位比例"), Piece.LocalTransform.GetScale3D().Equals(FVector::OneVector));
				continue;
			}

			++CreatedPrimitiveCount;
			const FBuildRenderCustomDataFragment* BurnData
				= Registry.FindFragment<FBuildRenderCustomDataFragment>(Entity);
				TestNull(TEXT("稳定 Cold 不预建 Burn Custom Data"), BurnData);
				int32 BurnCustomDataIndex = INDEX_NONE;
				TestTrue(TEXT("燃烧资格只存在于共享 Catalog Definition"),
					TryGetBuildCombustionConfiguration(*Definition, BurnCustomDataIndex)
						&& BurnCustomDataIndex == UCityBuildingPieceDefinition::BurnAmountCustomDataIndex);
				TestFalse(TEXT("每个种子部件未受击时不分配伤害状态"),
					Registry.HasFragment<FBuildDamageFragment>(Entity));
		}

		TestEqual(TEXT("展开后 Registry Entity 数精确等于配方 Piece 数"), Registry.GetEntityCount(),
			Recipe->GetPieceEntityCount());
		TestEqual(TEXT("配方 Door 计数与实际展开一致"), CreatedDoorCount, Recipe->GetDoorEntityCount());
		TestEqual(TEXT("除门外所有部件都是独立 primitive Entity"), CreatedPrimitiveCount,
			Recipe->GetPieceEntityCount() - Recipe->GetDoorEntityCount());

		TotalPieceEntityCount += Recipe->GetPieceEntityCount();
		TotalDoorEntityCount += Recipe->GetDoorEntityCount();
		RoundedSilhouettes.Add(FIntVector(FMath::RoundToInt(Recipe->GetNominalFootprintCentimeters().X / 10.0),
			FMath::RoundToInt(Recipe->GetNominalFootprintCentimeters().Y / 10.0),
			FMath::RoundToInt(Recipe->GetNominalHeightCentimeters() / 10.0)));
	}

	TestEqual(TEXT("全部 RecipeId 唯一"), RecipeIds.Num(), Archetypes.Num());
	TestEqual(TEXT("十二种当前结构配方合计复用八扇原始门"), TotalDoorEntityCount, 8);
	TestTrue(TEXT("十二种结构平均会展开为远多于一个 Entity"), TotalPieceEntityCount > Archetypes.Num() * 20);
	TestTrue(TEXT("原型库至少形成八种不同结构轮廓"), RoundedSilhouettes.Num() >= 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCityBuildingCatalogRegistrationTest,
	"ElementSandbox.BuildingCatalog.City.CatalogRegistersPiecesNotWholeStructures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCityBuildingCatalogRegistrationTest::RunTest(const FString& Parameters)
{
	UWorld* World
		= UWorld::CreateWorld(EWorldType::Game, false, TEXT("CityBuildingCatalogRegistration"), nullptr, true);
	TestNotNull(TEXT("创建聚落目录测试 World"), World);
	if (!World)
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building = World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog = World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	TestNotNull(TEXT("World 自动初始化 Building Subsystem"), Building);
	TestNotNull(TEXT("World 自动初始化 Building Catalog"), Catalog);

	if (Building && Catalog)
	{
		UTorchFixtureBuildingDefinition* MountedTorch =
			Cast<UTorchFixtureBuildingDefinition>(Building->FindDefinition(
				GetMountedTorchBuildingDefinitionId()));
		TestNotNull(TEXT("Catalog 注册火把的 MountedBuilding 形态"), MountedTorch);
		if (MountedTorch)
		{
			TestEqual(TEXT("挂墙火把由杆体与火焰两个静态 HISM Mesh Part 组成"),
				MountedTorch->MeshParts.Num(), 2);
			if (MountedTorch->MeshParts.Num() == 2)
			{
				const FBuildMeshPartDefinition& Shaft = MountedTorch->MeshParts[0];
				const FBuildMeshPartDefinition& Flame = MountedTorch->MeshParts[1];
				TestNotNull(TEXT("挂墙火把杆体 Mesh 有效"), Shaft.Mesh.Get());
				TestNotNull(TEXT("挂墙火把杆体材质有效"), Shaft.MaterialOverride.Get());
				TestNotNull(TEXT("挂墙火把火焰 Mesh 有效"), Flame.Mesh.Get());
				TestNotNull(TEXT("挂墙火把火焰材质有效"), Flame.MaterialOverride.Get());
				if (Shaft.Mesh && Shaft.MaterialOverride && Flame.Mesh && Flame.MaterialOverride)
				{
					TestEqual(TEXT("杆体使用 Cylinder Mesh"), Shaft.Mesh->GetPathName(),
						FString(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
					TestEqual(TEXT("杆体使用木质材质"), Shaft.MaterialOverride->GetPathName(),
						FString(TEXT("/Game/Building/Materials/M_BuildingBurnable.M_BuildingBurnable")));
					TestEqual(TEXT("火焰使用 Cone Mesh"), Flame.Mesh->GetPathName(),
						FString(TEXT("/Engine/BasicShapes/Cone.Cone")));
					TestEqual(TEXT("火焰使用火焰材质"), Flame.MaterialOverride->GetPathName(),
						FString(TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame")));
				}
				TestTrue(TEXT("杆体保持静态批量表现"),
					Shaft.PresentationPolicy == EBuildMeshPartPresentationPolicy::Static);
				TestTrue(TEXT("火焰保持静态批量表现"),
					Flame.PresentationPolicy == EBuildMeshPartPresentationPolicy::Static);
				TestEqual(TEXT("杆体 Surface 标签"), Shaft.SurfaceProfileId,
					FName(TEXT("Surface.Torch.Wood")));
				TestEqual(TEXT("火焰 Surface 标签"), Flame.SurfaceProfileId,
					FName(TEXT("Surface.Torch.Flame")));
			}
			TestEqual(TEXT("挂墙火把只为木杆声明近场碰撞"),
				MountedTorch->CollisionParts.Num(), 1);
			if (MountedTorch->CollisionParts.Num() == 1)
			{
				TestEqual(TEXT("挂墙火把碰撞跟随木杆 Mesh Part"),
					MountedTorch->CollisionParts[0].DrivenMeshPartId, 0);
			}
			int32 BurnCustomDataIndex = INDEX_NONE;
			TestTrue(TEXT("挂墙火把同时是普通可燃 Building Target"),
				TryGetBuildCombustionConfiguration(*MountedTorch, BurnCustomDataIndex)
					&& BurnCustomDataIndex == 0);
			EBuildFixedFireEmitterKind EmitterKind = EBuildFixedFireEmitterKind::FirePile;
			TestTrue(TEXT("挂墙火把显式登记为固定火源"),
				TryGetBuildFixedFireEmitterKind(MountedTorch->DefinitionId, EmitterKind));
			TestTrue(TEXT("挂墙火把固定火源种类可解析"),
				EmitterKind == EBuildFixedFireEmitterKind::MountedTorch);
		}

		UDoorBuildingDefinition* SettlementDoor
			= Cast<UDoorBuildingDefinition>(Building->FindDefinition(TEXT("Settlement.Door")));
		TestNotNull(TEXT("Catalog 注册聚落共用原始门 Definition"), SettlementDoor);
		if (SettlementDoor)
		{
			TestEqual(TEXT("聚落门保留原始七部件配置"), SettlementDoor->MeshParts.Num(), 7);
		}

		for (const ECityBuildingPieceKind Kind : GetDefaultCityPrimitivePieceKinds())
		{
			for (const FName SurfaceProfileId : GetDefaultCityPieceSurfaceProfileIds())
			{
				const FName DefinitionId = GetCityBuildingPieceDefinitionId(Kind, SurfaceProfileId);
				UCityBuildingPieceDefinition* Definition
					= Cast<UCityBuildingPieceDefinition>(Building->FindDefinition(DefinitionId));
				TestNotNull(FString::Printf(TEXT("Catalog 注册共享部件 %s"), *DefinitionId.ToString()), Definition);
				TestTrue(TEXT("Catalog getter 返回同一个 Kind+Surface Definition"),
					Definition
						&& Catalog->GetCityBuildingPieceDefinition(Kind, SurfaceProfileId) == Definition);
			}
		}

		for (const ECityBuildingArchetype Archetype : GetDefaultCityBuildingArchetypes())
		{
			TestNull(TEXT("Editor-only RecipeId 不注册进 Runtime Building Catalog"),
				Building->FindDefinition(GetCityBuildingRecipeId(Archetype)));
		}
		}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCityBuildingRealRoofFloorRegistrationAuditTest,
	"ElementSandbox.BuildingCatalog.City.RealRoofFloorRegistrationAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCityBuildingRealRoofFloorRegistrationAuditTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("CityBuildingRealRoofFloorRegistrationAudit"),
		nullptr,
		true);
	if (!TestNotNull(TEXT("创建真实 Recipe 审计 World"), World))
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UBuildingWorldSubsystem* Building = World->GetSubsystem<UBuildingWorldSubsystem>();
	UBuildingCatalogWorldSubsystem* Catalog = World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
	UCityBuildingRecipe* Recipe = NewObject<UCityBuildingRecipe>(World);
	if (!TestTrue(TEXT("初始化 Timber Cottage Recipe 与宿主 Subsystem"),
		Building && Catalog && Recipe
			&& Recipe->Initialize(ECityBuildingArchetype::TimberCottage)))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	const FName StoneSurface(TEXT("Surface.City.Stone"));
	const FName WoodSurface(TEXT("Surface.City.Wood"));
	const FCityBuildingPieceRecipe* FloorPiece = nullptr;
	const FCityBuildingPieceRecipe* RoofPiece = nullptr;
	const FCityBuildingPieceRecipe* DecorativePiece = nullptr;
	for (const FCityBuildingPieceRecipe& Piece : Recipe->GetPieces())
	{
		if (!FloorPiece
			&& Piece.Kind == ECityBuildingPieceKind::SolidBox
			&& Piece.SurfaceProfileId == StoneSurface
			&& Piece.LocalTransform.GetLocation().Z <= 50.0
			&& Piece.LocalTransform.GetScale3D().Z <= 0.75)
		{
			FloorPiece = &Piece;
		}
		if (!RoofPiece
			&& Piece.Kind == ECityBuildingPieceKind::SolidBox
			&& Piece.SurfaceProfileId == WoodSurface
			&& !Piece.LocalTransform.GetRotation().Equals(FQuat::Identity, UE_KINDA_SMALL_NUMBER)
			&& Piece.LocalTransform.GetScale3D().Z <= 0.75)
		{
			RoofPiece = &Piece;
		}
		if (!DecorativePiece && Piece.Kind == ECityBuildingPieceKind::DecorativeBox)
		{
			DecorativePiece = &Piece;
		}
	}
	TestNotNull(TEXT("真实 Recipe 中找到低矮 Solid 地板"), FloorPiece);
	TestNotNull(TEXT("真实 Recipe 中找到倾斜 Solid 屋顶"), RoofPiece);
	TestNotNull(TEXT("真实 Recipe 中找到明确 Decorative 部件"), DecorativePiece);

	struct FCreatedRecipePart final
	{
		const FCityBuildingPieceRecipe* Piece = nullptr;
		UCityBuildingPieceDefinition* Definition = nullptr;
		FBuildEntityHandle Entity;
	};
	auto CreatePart = [Building, Catalog](const FCityBuildingPieceRecipe* Piece)
	{
		FCreatedRecipePart Result;
		Result.Piece = Piece;
		if (Piece)
		{
			Result.Definition = Catalog->GetCityBuildingPieceDefinition(
				Piece->Kind,
				Piece->SurfaceProfileId);
			if (Result.Definition)
			{
				Result.Entity = Building->CreateEntity(
					*Result.Definition,
					Piece->LocalTransform);
			}
		}
		return Result;
	};
	const FCreatedRecipePart Floor = CreatePart(FloorPiece);
	const FCreatedRecipePart Roof = CreatePart(RoofPiece);
	const FCreatedRecipePart Decorative = CreatePart(DecorativePiece);
	if (!TestTrue(TEXT("真实地板、屋顶与 Decorative 部件均创建成功"),
		Floor.Entity.IsSet() && Roof.Entity.IsSet() && Decorative.Entity.IsSet()))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	for (const FCreatedRecipePart* SolidPart : {&Floor, &Roof})
	{
		const FBuildRenderPartShapeAudit Shape =
			Building->AuditRenderPartShape(SolidPart->Entity, 0);
		const FBuildRenderPartCollisionAudit BeforeSource =
			Building->AuditRenderPartCollision(SolidPart->Entity, 0);
		TestTrue(TEXT("真实 Solid Render Part 注册独立 Shape"),
			Shape.Status == EBuildRenderPartShapeAuditStatus::Registered);
		TestTrue(TEXT("真实 Solid 在 Source 覆盖前明确是 NotRequired"),
			BeforeSource.CollisionParts.Num() == 1
				&& BeforeSource.CollisionParts[0].Status
					== EBuildCollisionPartAuditStatus::NotRequired);
	}

	FBox RequiredBounds(ForceInit);
	FBox FloorBounds(ForceInit);
	FBox RoofBounds(ForceInit);
	const bool bResolvedBounds = Floor.Definition->TryCalculateCollisionPartWorldBounds(
		0, Floor.Piece->LocalTransform, {}, FloorBounds)
		&& Roof.Definition->TryCalculateCollisionPartWorldBounds(
			0, Roof.Piece->LocalTransform, {}, RoofBounds);
	TestTrue(TEXT("真实地板与斜屋顶可解析精确 Collision Bounds"), bResolvedBounds);
	if (bResolvedBounds)
	{
		RequiredBounds = FloorBounds + RoofBounds;
		FBuildCollisionSource Source;
		Source.SubjectLocation = RequiredBounds.GetCenter();
		Source.ImmediateBounds = RequiredBounds.ExpandBy(10.0);
		Source.CameraBounds = Source.ImmediateBounds;
		Source.PrefetchBounds = Source.ImmediateBounds;
		Source.RetentionBounds = RequiredBounds.ExpandBy(100.0);
		Source.Revision = 1;
		TestTrue(TEXT("真实 Recipe Collision Source 注册成功"),
			Building->RegisterCollisionSource(Source).IsSet());
		TestTrue(TEXT("真实屋顶与地板 Collision 投影成功"),
			Building->FlushCollisionChanges());
	}

	for (const FCreatedRecipePart* SolidPart : {&Floor, &Roof})
	{
		const FBuildRenderPartCollisionAudit Active =
			Building->AuditRenderPartCollision(SolidPart->Entity, 0);
		TestTrue(TEXT("真实 Solid 审计最终追踪到 Active Body"),
			Active.CollisionParts.Num() == 1
				&& Active.CollisionParts[0].Status
					== EBuildCollisionPartAuditStatus::ActiveBody
				&& Active.CollisionParts[0].ClusterKey.IsSet()
				&& Active.CollisionParts[0].Instance.IsSet());
	}

	const FBuildRenderPartShapeAudit DecorativeShape =
		Building->AuditRenderPartShape(Decorative.Entity, 0);
	const FBuildRenderPartCollisionAudit DecorativeCollision =
		Building->AuditRenderPartCollision(Decorative.Entity, 0);
	TestTrue(TEXT("Decorative Render Part 仍注册独立 Shape"),
		DecorativeShape.Status == EBuildRenderPartShapeAuditStatus::Registered);
	TestTrue(TEXT("Decorative 零碰撞被明确审计且未擅自补 Solid Body"),
		DecorativeCollision.CollisionParts.Num() == 1
			&& DecorativeCollision.CollisionParts[0].Status
				== EBuildCollisionPartAuditStatus::NoCollisionDefinition);
	TestEqual(TEXT("真实审计只为屋顶与地板创建两个 Body"),
		Building->GetActiveCollisionBodyCount(), 2);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
