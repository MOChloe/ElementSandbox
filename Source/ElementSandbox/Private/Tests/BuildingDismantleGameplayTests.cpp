#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Building/BuildingDismantleAuthorityService.h"
#include "Building/BuildingItemFeature.h"

#include "BuildingWorldSubsystem.h"
#include "Characters/ElementSandboxCharacter.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Definition/BuildingDefinition.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Focus/FocusQueryTypes.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Items/DemolitionToolItemDefinition.h"
#include "Items/ReclaimedBuildingItemDefinition.h"
#include "Misc/AutomationTest.h"
#include "Torch/TorchDefinition.h"

namespace ElementSandbox::BuildingDismantle::Tests
{
	struct FDismantleWorld final
	{
		FDismantleWorld()
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
				TEXT("BuildingDismantleAuthority"),
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
		}

		~FDismantleWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		TTuple<APawn*, UInventoryComponent*> MakePlayer(
			const TCHAR* InventoryName,
			const FVector& Location = FVector::ZeroVector) const
		{
			APlayerState* PlayerState = World->SpawnActor<APlayerState>();
			APawn* Pawn = World->SpawnActor<APawn>(Location, FRotator::ZeroRotator);
			Pawn->SetPlayerState(PlayerState);
			UInventoryComponent* Inventory = NewObject<UInventoryComponent>(
				PlayerState,
				InventoryName);
			PlayerState->AddInstanceComponent(Inventory);
			Inventory->RegisterComponent();
				return MakeTuple(Pawn, Inventory);
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Building = nullptr;
	};

	int32 CountQuantity(
		const UInventoryComponent& Inventory,
		const UItemDefinition& Definition)
	{
		int32 Quantity = 0;
		const auto CountSlots = [&Definition, &Quantity](
			const TArray<TObjectPtr<UItemInstance>>& Slots)
		{
			for (const UItemInstance* Item : Slots)
			{
				if (!Item || Item->GetDefinition().GetObject() != &Definition)
				{
					continue;
				}
				const UItemStackFeature* Stack = Item->FindFeature<UItemStackFeature>();
				Quantity += Stack ? Stack->GetQuantity() : 1;
			}
		};
		CountSlots(Inventory.GetQuickbarSlots());
		CountSlots(Inventory.GetBackpackSlots());
		return Quantity;
	}

	UItemInstance* FindFirstItem(
		const UInventoryComponent& Inventory,
		const UItemDefinition& Definition)
	{
		for (const TArray<TObjectPtr<UItemInstance>>* Slots : {
			&Inventory.GetQuickbarSlots(),
			&Inventory.GetBackpackSlots()})
		{
			for (UItemInstance* Item : *Slots)
			{
				if (Item && Item->GetDefinition().GetObject() == &Definition)
				{
					return Item;
				}
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingDismantleAuthorityTransactionTest,
	"ElementSandbox.Building.Dismantle.AuthorityTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingDismantleAuthorityTransactionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::BuildingDismantle::Tests;
	FDismantleWorld Harness;
	UBuildingDefinition* WallDefinition = Harness.Building
		? Harness.Building->FindDefinition(TEXT("WoodWall"))
		: nullptr;
	UBuildingDefinition* FloorDefinition = Harness.Building
		? Harness.Building->FindDefinition(TEXT("WoodFloor"))
		: nullptr;
	UBuildingDefinition* FirePileDefinition = Harness.Building
		? Harness.Building->FindDefinition(TEXT("FirePile"))
		: nullptr;
	UBuildingDefinition* MountedTorchDefinition = Harness.Building
		? Harness.Building->FindDefinition(GetMountedTorchBuildingDefinitionId())
		: nullptr;
	const FName CityPieceId = GetCityBuildingPieceDefinitionId(
		ECityBuildingPieceKind::SolidBox,
		TEXT("Surface.City.Wall"));
	UBuildingDefinition* CityPieceDefinition = Harness.Building
		? Harness.Building->FindDefinition(CityPieceId)
		: nullptr;
	UItemDefinition* WallItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"));
	UItemDefinition* FloorItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_WoodFloor.DA_WoodFloor"));
	UDemolitionToolItemDefinition* ToolDefinition =
		GetMutableDefault<UDemolitionToolItemDefinition>();
	UReclaimedBuildingItemDefinition* ReclaimedDefinition =
		GetMutableDefault<UReclaimedBuildingItemDefinition>();
	if (!Harness.Building || !WallDefinition || !FloorDefinition
		|| !FirePileDefinition || !MountedTorchDefinition || !CityPieceDefinition
		|| !WallItem || !FloorItem
		|| !ToolDefinition || !ReclaimedDefinition)
	{
		return false;
	}

	auto Player = Harness.MakePlayer(TEXT("DismantleInventory"));
	APawn* Pawn = Player.Get<0>();
	UInventoryComponent* Inventory = Player.Get<1>();
	TestTrue(TEXT("服务器发放拆除锤"),
		Inventory->GrantItemToQuickbar(ToolDefinition, 0));
	Inventory->SelectQuickbarSlot(0);

	double LastRequestTime = -DBL_MAX;
	TestEqual(TEXT("死亡玩家由拆除请求门禁拒绝"),
		FBuildingDismantleAuthorityService::TryBeginRequest(
			true, false, 0.0, LastRequestTime),
		EBuildingDismantleFailure::PlayerUnavailable);
	TestEqual(TEXT("背包打开时由拆除请求门禁拒绝"),
		FBuildingDismantleAuthorityService::TryBeginRequest(
			false, true, 0.0, LastRequestTime),
		EBuildingDismantleFailure::InventoryOpen);
	TestEqual(TEXT("合法拆除请求通过门禁"),
		FBuildingDismantleAuthorityService::TryBeginRequest(
			false, false, 0.0, LastRequestTime),
		EBuildingDismantleFailure::None);
	TestEqual(TEXT("过密拆除请求被限流"),
		FBuildingDismantleAuthorityService::TryBeginRequest(
			false, false, 0.01, LastRequestTime),
		EBuildingDismantleFailure::RateLimited);

	const FBuildEntityHandle Wall = Harness.Building->CreateEntity(
		*WallDefinition,
		FTransform(FVector(200.0, 0.0, 0.0)));
	const FWorldEntityId WallId = Harness.Building->GetWorldEntityId(Wall);
	TestEqual(TEXT("合法木墙拆除成功"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building, *Inventory, *Pawn, WallId, 500.0),
		EBuildingDismantleFailure::None);
	TestFalse(TEXT("拆除使用 GameplayDestroy 终结原 Building"),
		Harness.Building->IsEntityAlive(Wall));
	TestEqual(TEXT("拆除向背包返还一个木墙"),
		CountQuantity(*Inventory, *WallItem), 1);

	const FBuildEntityHandle FarWall = Harness.Building->CreateEntity(
		*WallDefinition,
		FTransform(FVector(2000.0, 0.0, 0.0)));
	TestEqual(TEXT("服务器拒绝远距离拆除"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building,
			*Inventory,
			*Pawn,
			Harness.Building->GetWorldEntityId(FarWall),
			500.0),
		EBuildingDismantleFailure::OutOfRange);
	TestTrue(TEXT("距离拒绝不删除目标"),
		Harness.Building->IsEntityAlive(FarWall));
	TestEqual(TEXT("距离拒绝不增加返还物"),
		CountQuantity(*Inventory, *WallItem), 1);

	const FBuildEntityHandle FirePile = Harness.Building->CreateEntity(
		*FirePileDefinition,
		FTransform(FVector(150.0, 0.0, 0.0)));
	TestEqual(TEXT("没有返还映射的 FirePile 不可拆"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building,
			*Inventory,
			*Pawn,
			Harness.Building->GetWorldEntityId(FirePile),
			500.0),
		EBuildingDismantleFailure::NotDismantleable);
	TestTrue(TEXT("不支持的目标保持存活"),
		Harness.Building->IsEntityAlive(FirePile));

	const FTransform CityPieceTransform(
		FRotator(12.0, 37.0, 8.0),
		FVector(200.0, 220.0, 120.0),
		FVector(3.0, 0.35, 1.75));
	const FBuildEntityHandle CityPiece = Harness.Building->CreateEntity(
		*CityPieceDefinition,
		CityPieceTransform);
	TestEqual(TEXT("AI 预置城镇构件按玩家建筑成功拆除"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building,
			*Inventory,
			*Pawn,
			Harness.Building->GetWorldEntityId(CityPiece),
			500.0),
		EBuildingDismantleFailure::None);
	TestFalse(TEXT("城镇构件原 Entity 已 GameplayDestroy"),
		Harness.Building->IsEntityAlive(CityPiece));
	UItemInstance* ReclaimedItem = FindFirstItem(*Inventory, *ReclaimedDefinition);
	const UBuildingItemFeature* ReclaimedFeature = ReclaimedItem
		? ReclaimedItem->FindFeature<UBuildingItemFeature>()
		: nullptr;
	TestTrue(TEXT("城镇构件返还不可堆叠的运行期 Building 道具"),
		ReclaimedFeature && CountQuantity(*Inventory, *ReclaimedDefinition) == 1);
	TestEqual(TEXT("返还道具保存原城镇 DefinitionId"),
		ReclaimedFeature ? ReclaimedFeature->GetBuildingDefinitionId() : NAME_None,
		CityPieceId);
	TestTrue(TEXT("返还道具丢弃旧位置但保留旋转与缩放"),
		ReclaimedFeature
		&& ReclaimedFeature->GetPlacementShapeTransform().GetLocation().IsNearlyZero()
		&& ReclaimedFeature->GetPlacementShapeTransform().GetRotation().Equals(
			CityPieceTransform.GetRotation(),
			0.001)
			&& ReclaimedFeature->GetPlacementShapeTransform().GetScale3D().Equals(
				CityPieceTransform.GetScale3D(),
				0.001));

	const FBuildEntityHandle MountedTorch = Harness.Building->CreateEntity(
		*MountedTorchDefinition,
		FTransform(FVector(150.0, 100.0, 0.0)));
	TestEqual(TEXT("挂墙火把可按独立 Building 回收"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building,
			*Inventory,
			*Pawn,
			Harness.Building->GetWorldEntityId(MountedTorch),
			500.0),
		EBuildingDismantleFailure::None);
	TestFalse(TEXT("挂墙火把原 Entity 已 GameplayDestroy"),
		Harness.Building->IsEntityAlive(MountedTorch));
	TestEqual(TEXT("挂墙火把返还一个不可堆叠构件"),
		CountQuantity(*Inventory, *ReclaimedDefinition), 2);

	const FBuildEntityHandle VetoedFloor = Harness.Building->CreateEntity(
		*FloorDefinition,
		FTransform(FVector(-200.0, 0.0, 0.0)));
	const FDelegateHandle VetoHandle = Harness.Building->OnEntityPreDestroy().AddLambda(
		[VetoedFloor](const FBuildEntityHandle Entity, bool& bCanDestroy)
		{
			if (Entity == VetoedFloor)
			{
				bCanDestroy = false;
			}
		});
	TestEqual(TEXT("跨域监听方否决时拆除失败"),
		FBuildingDismantleAuthorityService::TryDismantle(
			*Harness.Building,
			*Inventory,
			*Pawn,
			Harness.Building->GetWorldEntityId(VetoedFloor),
			500.0),
		EBuildingDismantleFailure::DestroyFailed);
	TestTrue(TEXT("否决后 Building 保持存活"),
		Harness.Building->IsEntityAlive(VetoedFloor));
	TestEqual(TEXT("否决后完整回滚已加入的地板返还"),
		CountQuantity(*Inventory, *FloorItem), 0);
	Harness.Building->OnEntityPreDestroy().Remove(VetoHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingDismantleFocusAndPrimaryUseTest,
	"ElementSandbox.Building.Dismantle.FocusAndPrimaryUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingDismantleFocusAndPrimaryUseTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::BuildingDismantle::Tests;
	FDismantleWorld Harness;
	UBuildingDefinition* WallDefinition = Harness.Building
		? Harness.Building->FindDefinition(TEXT("WoodWall"))
		: nullptr;
	UBuildingDefinition* FirePileDefinition = Harness.Building
		? Harness.Building->FindDefinition(TEXT("FirePile"))
		: nullptr;
	UBuildingDefinition* MountedTorchDefinition = Harness.Building
		? Harness.Building->FindDefinition(GetMountedTorchBuildingDefinitionId())
		: nullptr;
	const FName CityPieceId = GetCityBuildingPieceDefinitionId(
		ECityBuildingPieceKind::SolidBox,
		TEXT("Surface.City.Wall"));
	UBuildingDefinition* CityPieceDefinition = Harness.Building
		? Harness.Building->FindDefinition(CityPieceId)
		: nullptr;
	UItemDefinition* WallItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_WoodWall.DA_WoodWall"));
	AElementSandboxPlayerState* PlayerState =
		Harness.World->SpawnActor<AElementSandboxPlayerState>();
	AElementSandboxPlayerController* Controller =
		Harness.World->SpawnActor<AElementSandboxPlayerController>();
	AElementSandboxCharacter* Character =
		Harness.World->SpawnActor<AElementSandboxCharacter>();
	if (!Harness.Building || !WallDefinition || !FirePileDefinition || !MountedTorchDefinition
		|| !CityPieceDefinition || !WallItem
		|| !PlayerState || !Controller || !Character)
	{
		return false;
	}

	// 先让 Focus 组件在没有 Inventory 的控制器上完成注册，避免无 ULocalPlayer 的
	// Automation World 触发 HUD CreateWidget；随后再装配真实 Gameplay 状态。
	if (!Controller->HasActorBegunPlay())
	{
		Controller->DispatchBeginPlay();
	}
	Controller->SetPlayerState(PlayerState);
	Controller->Possess(Character);
	UInventoryComponent* Inventory = PlayerState->GetInventoryComponent();
	const UDemolitionToolItemDefinition* ToolDefinition =
		GetDefault<UDemolitionToolItemDefinition>();
	TestTrue(TEXT("Focus 场景发放并选中拆除锤"),
		Inventory && ToolDefinition
		&& Inventory->GrantItemToQuickbar(
			const_cast<UDemolitionToolItemDefinition*>(ToolDefinition), 0));
	if (!Inventory || !ToolDefinition)
	{
		return false;
	}
	Inventory->SelectQuickbarSlot(0);

	const FBuildEntityHandle CityPiece = Harness.Building->CreateEntity(
		*CityPieceDefinition,
		FTransform(
			FQuat::Identity,
			FVector(100.0, 0.0, 0.0),
			FVector(1.5, 1.0, 1.0)));
	const FBuildEntityHandle Wall = Harness.Building->CreateEntity(
		*WallDefinition,
		FTransform(FVector(260.0, 0.0, 0.0)));
	const FBuildEntityHandle MountedTorch = Harness.Building->CreateEntity(
		*MountedTorchDefinition,
		FTransform(FVector(180.0, 0.0, 0.0)));
	UFocusHostComponent* Host =
		Controller->FindComponentByClass<UFocusHostComponent>();
	FFocusQueryContext Context;
	// 视线从火把细杆/火焰原始 Bounds 旁边掠过，但仍落在小目标的拆除辅助范围内；
	// 没有辅助时射线会穿到后方木墙，表现成火把永远拆不掉。
	Context.ViewOrigin = FVector(0.0, 15.0, 50.0);
	Context.ViewDirection = FVector::ForwardVector;
	if (Host)
	{
		Host->EvaluateFocus(Context);
	}
	FFocusInteractionPrompt Prompt;
	TestTrue(TEXT("拆除锤会聚焦 AI 预置的前景城镇构件"),
		Host && Host->TryResolveFocusedPrompt(Prompt));
	TestTrue(TEXT("城镇构件提示明确左键与回收道具"),
		Prompt.Text.ToString().Contains(TEXT("左键拆除"))
		&& Prompt.Text.ToString().Contains(TEXT("回收建筑构件")));
	TestTrue(TEXT("左键 Primary Use 完整进入城镇构件 Authority 拆除事务"),
		Host && Host->HandlePrimaryUse());
	TestFalse(TEXT("左键终结 AI 预置城镇构件"),
		Harness.Building->IsEntityAlive(CityPiece));

	// 第一锤已更新服务器限流时间；推进超过 0.08 秒后再验证下一次独立左键。
	Harness.World->Tick(LEVELTICK_All, 0.1f);
	if (Host)
	{
		Host->EvaluateFocus(Context);
	}
	TestTrue(TEXT("没有 Collision Part 的挂墙火把仍可由 Mesh Part 聚焦"),
		Host && Host->TryResolveFocusedPrompt(Prompt));
	TestTrue(TEXT("挂墙火把提示明确左键与回收构件"),
		Prompt.Text.ToString().Contains(TEXT("左键拆除"))
		&& Prompt.Text.ToString().Contains(TEXT("回收建筑构件")));
	TestTrue(TEXT("挂墙火把 Focus Primary Use 进入 Authority 拆除事务"),
		Host && Host->HandlePrimaryUse());
	TestFalse(TEXT("Focus Primary Use 终结挂墙火把"),
		Harness.Building->IsEntityAlive(MountedTorch));

	Harness.World->Tick(LEVELTICK_All, 0.1f);
	if (Host)
	{
		Host->EvaluateFocus(Context);
	}
	TestTrue(TEXT("后方木墙成为可拆 Focus"),
		Host && Host->TryResolveFocusedPrompt(Prompt));
	TestTrue(TEXT("木墙提示明确左键与返还物"),
		Prompt.Text.ToString().Contains(TEXT("左键拆除"))
		&& Prompt.Text.ToString().Contains(TEXT("木墙")));
	TestTrue(TEXT("Focus Primary Use 完整进入 Authority 拆除事务"),
		Host && Host->HandlePrimaryUse());
	TestFalse(TEXT("Primary Use 终结木墙"),
		Harness.Building->IsEntityAlive(Wall));
	TestEqual(TEXT("Primary Use 把木墙返还背包"),
		CountQuantity(*Inventory, *WallItem), 1);
	return true;
}

#endif
