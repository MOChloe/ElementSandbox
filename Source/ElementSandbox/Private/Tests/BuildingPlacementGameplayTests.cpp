#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Building/BuildingPlacementAuthorityService.h"
#include "Building/BuildingItemFeature.h"
#include "Building/BuildingPlacementResolver.h"

#include "BuildingWorldSubsystem.h"
#include "Collision/BuildCollisionHost.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Definition/BuildingDefinition.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Misc/AutomationTest.h"
#include "Tests/BuildingPlacementTestTypes.h"

namespace ElementSandbox::BuildingPlacement::Tests
{
	struct FPlacementIntentWorld final
	{
		FPlacementIntentWorld()
		{
			UWorld::InitializationValues InitializationValues;
			InitializationValues
				.CreatePhysicsScene(true)
				.ShouldSimulatePhysics(false)
				.EnableTraceCollision(true)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE,
				false,
				TEXT("BuildingPlacementIntent"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NM_Standalone);
			World->InitWorld(InitializationValues);
			World->UpdateWorldComponents(true, false);
			Building = World->GetSubsystem<UBuildingWorldSubsystem>();
			Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		~FPlacementIntentWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		AStaticMeshActor* SpawnSurface(
			const FVector& Location,
			const FVector& Scale,
			const FRotator& Rotation = FRotator::ZeroRotator)
		{
			AStaticMeshActor* Surface = World->SpawnActor<AStaticMeshActor>();
			Surface->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Surface->GetStaticMeshComponent()->SetCollisionEnabled(
				ECollisionEnabled::QueryAndPhysics);
			Surface->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Block);
			Surface->SetActorTransform(FTransform(Rotation, Location, Scale));
			Surface->GetStaticMeshComponent()->RecreatePhysicsState();
			return Surface;
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Building = nullptr;
		UStaticMesh* Cube = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingPlacementAuthorityTransactionTest,
	"ElementSandbox.Building.Placement.AuthorityTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingPlacementAuthorityTransactionTest::RunTest(
	const FString& Parameters)
{
	using namespace ElementSandbox::BuildingPlacement::Tests;
	FPlacementIntentWorld Harness;
	UItemDefinition* WallItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"));
	if (!Harness.Building || !Harness.Cube || !WallItem)
	{
		return false;
	}
	Harness.SpawnSurface(
		FVector(0.0, 0.0, -50.0),
		FVector(20.0, 20.0, 1.0));
	double LastRequestTime = -DBL_MAX;
	TestEqual(TEXT("死亡玩家由服务器请求门禁拒绝"),
		FBuildingPlacementAuthorityService::TryBeginRequest(
			true, false, 0.0, LastRequestTime),
		EBuildPlacementFailure::PlayerUnavailable);
	TestEqual(TEXT("背包打开时由服务器请求门禁拒绝"),
		FBuildingPlacementAuthorityService::TryBeginRequest(
			false, true, 0.0, LastRequestTime),
		EBuildPlacementFailure::InventoryOpen);
	TestEqual(TEXT("首个合法请求通过频率门禁"),
		FBuildingPlacementAuthorityService::TryBeginRequest(
			false, false, 0.0, LastRequestTime),
		EBuildPlacementFailure::None);
	TestEqual(TEXT("过密摆放请求被限流"),
		FBuildingPlacementAuthorityService::TryBeginRequest(
			false, false, 0.01, LastRequestTime),
		EBuildPlacementFailure::RateLimited);
	TestEqual(TEXT("超过最小间隔后再次允许"),
		FBuildingPlacementAuthorityService::TryBeginRequest(
			false, false, 0.10, LastRequestTime),
		EBuildPlacementFailure::None);

	auto MakeInventory = [&Harness](const TCHAR* ComponentName)
	{
		APlayerState* PlayerState = Harness.World->SpawnActor<APlayerState>();
		APawn* Pawn = Harness.World->SpawnActor<APawn>();
		Pawn->SetPlayerState(PlayerState);
		UInventoryComponent* Inventory = NewObject<UInventoryComponent>(
			PlayerState,
			ComponentName);
		PlayerState->AddInstanceComponent(Inventory);
		Inventory->RegisterComponent();
		return TTuple<APawn*, UInventoryComponent*>(Pawn, Inventory);
	};
	const auto IsLiveMutationReady = [](const FVector&) { return true; };

	auto FirstPlayer = MakeInventory(TEXT("FirstPlacementInventory"));
	TestTrue(TEXT("第一名玩家获得两个木墙"),
		FirstPlayer.Get<1>()->GrantItemToQuickbar(WallItem, 0, 2));
	FirstPlayer.Get<1>()->SelectQuickbarSlot(0);
	const EBuildPlacementFailure StreamingNotReadyResult =
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*FirstPlayer.Get<1>(),
			*FirstPlayer.Get<0>(),
			0,
			FVector(400.0, 0.0, 0.0),
			FVector(400.0, 0.0, 0.0),
			0,
			[](const FVector&) { return false; });
	TestEqual(TEXT("目标 Chunk 尚无 ACK 基线时拒绝摆放"),
		StreamingNotReadyResult,
		EBuildPlacementFailure::StreamingNotReady);
	UItemInstance* UnchangedFirstStack = FirstPlayer.Get<1>()->GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 0));
	TestTrue(TEXT("后台流送拒绝发生在物品扣除之前"),
		UnchangedFirstStack
		&& UnchangedFirstStack->FindFeature<UItemStackFeature>()->GetQuantity() == 2);
	TestEqual(TEXT("后台流送拒绝不创建 Building Entity"),
		Harness.Building->GetSpatialIndex().GetEntityCount(), 0);
	const EBuildPlacementFailure FirstResult =
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*FirstPlayer.Get<1>(),
			*FirstPlayer.Get<0>(),
			0,
			FVector(400.0, 0.0, 0.0),
			FVector(400.0, 0.0, 0.0),
			0,
			IsLiveMutationReady);
	TestEqual(TEXT("服务器接受合法摆放"),
		FirstResult,
		EBuildPlacementFailure::None);
	UItemInstance* FirstStack = FirstPlayer.Get<1>()->GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 0));
	TestTrue(TEXT("成功摆放只扣除一个物品"),
		FirstStack
		&& FirstStack->FindFeature<UItemStackFeature>()->GetQuantity() == 1);
	TestEqual(TEXT("成功摆放创建一个权威 Building Entity"),
		Harness.Building->GetSpatialIndex().GetEntityCount(), 1);

	auto SecondPlayer = MakeInventory(TEXT("SecondPlacementInventory"));
	TestTrue(TEXT("第二名玩家获得两个木墙"),
		SecondPlayer.Get<1>()->GrantItemToQuickbar(WallItem, 0, 2));
	SecondPlayer.Get<1>()->SelectQuickbarSlot(0);
	const EBuildPlacementFailure ConcurrentResult =
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*SecondPlayer.Get<1>(),
			*SecondPlayer.Get<0>(),
			0,
			FVector(400.0, 0.0, 0.0),
			FVector(400.0, 0.0, 0.0),
			0,
			IsLiveMutationReady);
	TestNotEqual(TEXT("旧预览不会被服务器重解释为其他层高并成功"),
		ConcurrentResult,
		EBuildPlacementFailure::None);
	UItemInstance* SecondStack = SecondPlayer.Get<1>()->GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 0));
	TestTrue(TEXT("碰撞拒绝不扣第二名玩家物品"),
		SecondStack
		&& SecondStack->FindFeature<UItemStackFeature>()->GetQuantity() == 2);
	TestEqual(TEXT("被拒的旧预览不创建第二个 Building Entity"),
		Harness.Building->GetSpatialIndex().GetEntityCount(), 1);
	const EBuildPlacementFailure FarResult =
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*SecondPlayer.Get<1>(),
			*SecondPlayer.Get<0>(),
			0,
			FVector(900.0, 0.0, 0.0),
			FVector(900.0, 0.0, 0.0),
			0,
			IsLiveMutationReady);
	TestEqual(TEXT("服务器拒绝超过最大距离的请求"),
		FarResult,
		EBuildPlacementFailure::OutOfRange);

	UItemDefinition* StickItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Stick.DA_Stick"));
	auto WrongItemPlayer = MakeInventory(TEXT("WrongPlacementInventory"));
	TestTrue(TEXT("发放非建造木棍"),
		WrongItemPlayer.Get<1>()->GrantItemToQuickbar(StickItem, 0, 1));
	WrongItemPlayer.Get<1>()->SelectQuickbarSlot(0);
	TestEqual(TEXT("服务器不把普通物品当作建造物"),
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*WrongItemPlayer.Get<1>(),
			*WrongItemPlayer.Get<0>(),
			0,
			FVector(0.0, -400.0, 0.0),
			FVector(0.0, -400.0, 0.0),
			0,
			IsLiveMutationReady),
		EBuildPlacementFailure::NoBuildItem);

	UItemDefinition* UnknownBuildingItem = NewObject<UItemDefinition>(Harness.World);
	UItemStackFeature* UnknownStackTemplate =
		NewObject<UItemStackFeature>(UnknownBuildingItem);
	UnknownStackTemplate->MaxStackSize = 20;
	UBuildingItemFeature* UnknownBuildingTemplate =
		NewObject<UBuildingItemFeature>(UnknownBuildingItem);
	UnknownBuildingTemplate->BuildingDefinitionId = TEXT("Building.Unknown");
	UnknownBuildingItem->FeatureTemplates = {
		UnknownStackTemplate,
		UnknownBuildingTemplate};
	auto UnknownDefinitionPlayer = MakeInventory(TEXT("UnknownDefinitionInventory"));
	TestTrue(TEXT("发放指向未知 Definition 的测试物品"),
		UnknownDefinitionPlayer.Get<1>()->GrantItemToQuickbar(
			UnknownBuildingItem, 0, 1));
	UnknownDefinitionPlayer.Get<1>()->SelectQuickbarSlot(0);
	TestEqual(TEXT("服务器拒绝未知 Building DefinitionId"),
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*UnknownDefinitionPlayer.Get<1>(),
			*UnknownDefinitionPlayer.Get<0>(),
			0,
			FVector(0.0, -400.0, 0.0),
			FVector(0.0, -400.0, 0.0),
			0,
			IsLiveMutationReady),
		EBuildPlacementFailure::MissingDefinition);

	UFailingBuildingPlacementDefinition* FailingDefinition =
		NewObject<UFailingBuildingPlacementDefinition>(Harness.World);
	FailingDefinition->DefinitionId = TEXT("Building.Test.CreateFailure");
	const FTransform CenteredPart(FVector(0.0, 0.0, 50.0));
	FBuildMeshPartDefinition& MeshPart =
		FailingDefinition->MeshParts.AddDefaulted_GetRef();
	MeshPart.Mesh = Harness.Cube;
	MeshPart.LocalTransform = CenteredPart;
	FBuildCollisionPartDefinition& CollisionPart =
		FailingDefinition->CollisionParts.AddDefaulted_GetRef();
	CollisionPart.CollisionMesh = Harness.Cube;
	CollisionPart.LocalTransform = CenteredPart;
	TestTrue(TEXT("注册创建失败测试 Definition"),
		Harness.Building->RegisterDefinition(*FailingDefinition));

	UItemDefinition* FailingItem = NewObject<UItemDefinition>(Harness.World);
	UItemStackFeature* StackTemplate = NewObject<UItemStackFeature>(FailingItem);
	StackTemplate->MaxStackSize = 20;
	UBuildingItemFeature* BuildingTemplate =
		NewObject<UBuildingItemFeature>(FailingItem);
	BuildingTemplate->BuildingDefinitionId = FailingDefinition->DefinitionId;
	FailingItem->FeatureTemplates = {StackTemplate, BuildingTemplate};
	auto RollbackPlayer = MakeInventory(TEXT("RollbackPlacementInventory"));
	TestTrue(TEXT("发放两个创建失败测试物品"),
		RollbackPlayer.Get<1>()->GrantItemToQuickbar(FailingItem, 0, 2));
	RollbackPlayer.Get<1>()->SelectQuickbarSlot(0);
	const EBuildPlacementFailure FailedCreateResult =
		FBuildingPlacementAuthorityService::TryPlace(
			*Harness.World,
			*Harness.Building,
			*RollbackPlayer.Get<1>(),
			*RollbackPlayer.Get<0>(),
			0,
			FVector(0.0, 400.0, 0.0),
			FVector(0.0, 400.0, 0.0),
			0,
			IsLiveMutationReady);
	TestEqual(TEXT("Definition 创建失败向调用方返回明确结果"),
		FailedCreateResult,
		EBuildPlacementFailure::CreateFailed);
	UItemInstance* RestoredStack = RollbackPlayer.Get<1>()->GetItem(
		FInventorySlotAddress(EInventoryContainer::Quickbar, 0));
	TestTrue(TEXT("创建失败完整恢复扣除数量"),
		RestoredStack
		&& RestoredStack->FindFeature<UItemStackFeature>()->GetQuantity() == 2);
	TestEqual(TEXT("创建失败恢复选中快捷栏"),
		RollbackPlayer.Get<1>()->GetSelectedQuickbarIndex(), 0);
	TestEqual(TEXT("创建失败没有留下半初始化 Building"),
		Harness.Building->GetSpatialIndex().GetEntityCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingPlacementIntentResolutionTest,
	"ElementSandbox.Building.Placement.IntentResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingPlacementIntentResolutionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::BuildingPlacement::Tests;
	FPlacementIntentWorld Harness;
	UBuildingDefinition* Wall = Harness.Building
		? Harness.Building->FindDefinition(TEXT("WoodWall"))
		: nullptr;
	UBuildingDefinition* DecorativePiece = Harness.Building
		? Harness.Building->FindDefinition(GetCityBuildingPieceDefinitionId(
			ECityBuildingPieceKind::DecorativeBox,
			TEXT("Surface.City.Wood")))
		: nullptr;
	UBuildingDefinition* Floor = Harness.Building
		? Harness.Building->FindDefinition(TEXT("WoodFloor"))
		: nullptr;
	TestNotNull(TEXT("Catalog 在客户端与服务器都注册 WoodWall"), Wall);
	TestNotNull(TEXT("Catalog 在客户端与服务器都注册 WoodFloor"), Floor);
	TestNotNull(TEXT("Catalog 注册可回收的无碰撞城镇装饰构件"), DecorativePiece);
	if (!Harness.Building || !Harness.Cube || !Wall || !Floor || !DecorativePiece)
	{
		return false;
	}

	Harness.SpawnSurface(
		FVector(0.0, 0.0, -50.0),
		FVector(20.0, 20.0, 1.0));
	FBuildPlacementEvaluation Evaluation;
	TestTrue(TEXT("共享解析器处理合法地面意图"),
		FBuildingPlacementResolver::ResolveIntent(
			*Harness.World,
			*Harness.Building,
			*Wall,
			FVector(149.0, 151.0, 0.0),
			FVector(149.0, 151.0, 0.0),
			FTransform::Identity,
			5,
			FVector(100.0, 200.0, 0.0),
			nullptr,
			Evaluation));
	TestTrue(TEXT("合法地面候选允许摆放"), Evaluation.IsAllowed());
	TestTrue(TEXT("XY 保留准星命中的自由地形落点"),
		Evaluation.ResolvedTransform.GetLocation().Equals(
			FVector(149.0, 151.0, 0.0),
			0.1));
	TestTrue(TEXT("Yaw Quarter Turns 归一化为 90 度"),
		FMath::IsNearlyEqual(
			Evaluation.ResolvedTransform.Rotator().Yaw,
			90.0,
			0.1));
	TestTrue(TEXT("普通建造物品的 Identity 形态保持单位 Scale"),
		Evaluation.ResolvedTransform.GetScale3D().Equals(FVector::OneVector));

	const FTransform ReclaimedShape(
		FRotator(0.0, 35.0, 20.0),
		FVector::ZeroVector,
		FVector(2.0, 0.5, 1.5));
	const FVector ReclaimedSurface(-200.0, -200.0, 0.0);
	FBuildPlacementEvaluation ReclaimedPreview;
	TestTrue(TEXT("客户端先用共享规则解析回收构件预览原点"),
		FBuildingPlacementResolver::ResolveCandidateTransform(
			*Harness.World,
			*Harness.Building,
			*Wall,
			ReclaimedSurface,
			ReclaimedShape,
			1,
			nullptr,
			ReclaimedPreview)
		&& ReclaimedPreview.IsAllowed());
	FBuildPlacementEvaluation ReclaimedEvaluation;
	TestTrue(TEXT("共享解析器接受服务器背包中的回收构件形态"),
		FBuildingPlacementResolver::ResolveIntent(
			*Harness.World,
			*Harness.Building,
			*Wall,
			ReclaimedSurface,
			ReclaimedPreview.ResolvedTransform.GetLocation(),
			ReclaimedShape,
			1,
			FVector(-200.0, -200.0, 0.0),
			nullptr,
			ReclaimedEvaluation));
	TestTrue(TEXT("回收构件的非单位 Scale 被原样恢复"),
		ReclaimedEvaluation.ResolvedTransform.GetScale3D().Equals(
			ReclaimedShape.GetScale3D(),
			0.001));
	TestTrue(TEXT("回收构件的原倾角与玩家追加 Yaw 共同进入候选 Transform"),
		ReclaimedEvaluation.ResolvedTransform.GetRotation().Equals(
			(ReclaimedShape * FTransform(FRotator(0.0, 90.0, 0.0))).GetRotation(),
			0.001));

	const FVector DecorativeSurface(-400.0, 300.0, 0.0);
	const FTransform DecorativeShape(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(1.5, 0.25, 0.75));
	FBuildPlacementEvaluation DecorativePreview;
	TestTrue(TEXT("客户端先用共享规则解析无碰撞装饰构件预览原点"),
		FBuildingPlacementResolver::ResolveCandidateTransform(
			*Harness.World,
			*Harness.Building,
			*DecorativePiece,
			DecorativeSurface,
			DecorativeShape,
			0,
			nullptr,
			DecorativePreview)
		&& DecorativePreview.IsAllowed());
	FBuildPlacementEvaluation DecorativeEvaluation;
	TestTrue(TEXT("没有 Collision Part 的回收装饰构件仍可通过权威摆放"),
		FBuildingPlacementResolver::ResolveIntent(
			*Harness.World,
			*Harness.Building,
			*DecorativePiece,
			DecorativeSurface,
			DecorativePreview.ResolvedTransform.GetLocation(),
			DecorativeShape,
			0,
			FVector(-400.0, 300.0, 0.0),
			nullptr,
			DecorativeEvaluation));
	TestTrue(TEXT("装饰构件没有被偷加 Solid 碰撞也不会被规则误拒绝"),
		DecorativeEvaluation.IsAllowed()
		&& DecorativePiece->CollisionParts.IsEmpty());

	const FVector ExistingFloorLocation(500.0, 500.0, 0.0);
	TestTrue(TEXT("创建地板叠放的下层权威实体"),
		Harness.Building->CreateEntity(
			*Floor,
			FTransform(ExistingFloorLocation)).IsSet());
	ABuildCollisionHost* CollisionHost = Harness.Building->GetCollisionHost();
	TestNotNull(TEXT("测试 World 存在可丢弃 Collision Host"), CollisionHost);
	if (CollisionHost)
	{
		CollisionHost->SetActorEnableCollision(false);
	}
	FCollisionQueryParams SurfaceQuery(TEXT("BuildingLogicalSurfaceTest"), false);
	FBuildPlacementSurfaceHit ViewHit;
	TestTrue(TEXT("Collision Host 关闭后仍可从 Building ECS 命中地板侧面"),
		Harness.Building->QueryPlacementSurface(
			FVector(500.0, 0.0, 10.0),
			FVector(500.0, 1000.0, 10.0),
			SurfaceQuery,
			ViewHit));
	TestTrue(TEXT("视线命中明确来自 Building 逻辑几何"),
		ViewHit.bBuildingSurface);

	FBuildPlacementEvaluation StackedFloorEvaluation;
	TestTrue(TEXT("共享解析器可把第二块地板解析到现有地板顶面"),
		FBuildingPlacementResolver::ResolveIntent(
			*Harness.World,
			*Harness.Building,
			*Floor,
			ExistingFloorLocation + FVector(0.0, 0.0, 10.0),
			ExistingFloorLocation + FVector(0.0, 0.0, 20.0),
			FTransform::Identity,
			0,
			ExistingFloorLocation,
			nullptr,
			StackedFloorEvaluation));
	TestTrue(TEXT("地板相切叠放合法且不会穿入下层"),
		StackedFloorEvaluation.IsAllowed()
		&& FMath::IsNearlyEqual(
			StackedFloorEvaluation.ResolvedTransform.GetLocation().Z,
			20.0,
			0.1));
	FBuildPlacementEvaluation DuplicateFloorEvaluation;
	Harness.Building->EvaluatePlacement(
		*Floor,
		FTransform(ExistingFloorLocation),
		ExistingFloorLocation,
		500.0,
		DuplicateFloorEvaluation);
	TestEqual(TEXT("无 Physics Scene Query Body 时同位地板仍被逻辑几何拒绝"),
		DuplicateFloorEvaluation.Failure,
		EBuildPlacementFailure::BlockedByBuilding);
	if (CollisionHost)
	{
		CollisionHost->SetActorEnableCollision(true);
	}

	FBuildPlacementEvaluation FarEvaluation;
	FBuildingPlacementResolver::ResolveIntent(
		*Harness.World,
		*Harness.Building,
		*Wall,
		FVector(900.0, 0.0, 0.0),
		FVector(900.0, 0.0, 0.0),
		FTransform::Identity,
		0,
		FVector::ZeroVector,
		nullptr,
		FarEvaluation);
	TestEqual(TEXT("超过 500cm 由共享规则拒绝"),
		FarEvaluation.Failure,
		EBuildPlacementFailure::OutOfRange);

	FBuildPlacementEvaluation MissingSurface;
	FBuildingPlacementResolver::ResolveIntent(
		*Harness.World,
		*Harness.Building,
		*Wall,
		FVector(0.0, 5000.0, 0.0),
		FVector(0.0, 5000.0, 0.0),
		FTransform::Identity,
		0,
		FVector(0.0, 5000.0, 0.0),
		nullptr,
		MissingSurface);
	TestEqual(TEXT("没有支撑面明确拒绝"),
		MissingSurface.Failure,
		EBuildPlacementFailure::NoSurface);

	Harness.SpawnSurface(
		FVector(0.0, 3000.0, 0.0),
		FVector(2.0, 2.0, 0.2),
		FRotator(50.0, 0.0, 0.0));
	FBuildPlacementEvaluation SteepEvaluation;
	FBuildingPlacementResolver::ResolveIntent(
		*Harness.World,
		*Harness.Building,
		*Wall,
		FVector(0.0, 3000.0, 0.0),
		FVector(0.0, 3000.0, 0.0),
		FTransform::Identity,
		0,
		FVector(0.0, 3000.0, 0.0),
		nullptr,
		SteepEvaluation);
	TestEqual(TEXT("超过 45 度坡面拒绝"),
		SteepEvaluation.Failure,
		EBuildPlacementFailure::SurfaceTooSteep);
	return true;
}

#endif
