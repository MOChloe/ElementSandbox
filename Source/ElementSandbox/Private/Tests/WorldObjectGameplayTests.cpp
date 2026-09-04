#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ElementSandboxCharacter.h"
#include "Components/PrimitiveComponent.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Equipment/EquipmentComponent.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "Item/ItemDefinition.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemInstance.h"
#include "Items/StickEquippedItemActor.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tests/WorldObjectGameplayTestTypes.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectEquipmentBridgeComponent.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupResolver.h"
#include "WorldObjects/CharcoalWorldObjectDefinition.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/WorldObjectFocusTarget.h"

namespace ElementSandbox::WorldObjects::GameplayTests
{
	void AdvancePostActorFrame(UWorld& World, const float DeltaSeconds = 1.0f / 60.0f)
	{
		++GFrameCounter;
		FWorldDelegates::OnWorldPostActorTick.Broadcast(
			&World, LEVELTICK_All, DeltaSeconds);
	}

	struct FGameplayWorld final
	{
		FGameplayWorld()
		{
			World = UWorld::CreateWorld(
				EWorldType::Game,
				false,
				TEXT("WorldObjectGameplay"),
				nullptr,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			PlayerState = World->SpawnActor<AElementSandboxPlayerState>();
			Controller = World->SpawnActor<AElementSandboxPlayerController>();
			Character = World->SpawnActor<AElementSandboxCharacter>();
			Controller->SetPlayerState(PlayerState);
			Controller->Possess(Character);
			if (!Character->HasActorBegunPlay())
			{
				Character->DispatchBeginPlay();
			}
		}

		~FGameplayWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		AElementSandboxPlayerState* PlayerState = nullptr;
		AElementSandboxPlayerController* Controller = nullptr;
		AElementSandboxCharacter* Character = nullptr;
	};

	int32 CountInventoryItems(const UInventoryComponent& Inventory)
	{
		int32 Count = 0;
		for (const UItemInstance* Item : Inventory.GetQuickbarSlots())
		{
			Count += IsValid(Item) ? 1 : 0;
		}
		for (const UItemInstance* Item : Inventory.GetBackpackSlots())
		{
			Count += IsValid(Item) ? 1 : 0;
		}
		return Count;
	}

	int32 CountInventoryQuantity(
		const UInventoryComponent& Inventory,
		const UItemDefinition& Definition)
	{
		int32 Quantity = 0;
		auto CountSlots = [&Definition, &Quantity](
			const TArray<TObjectPtr<UItemInstance>>& Slots)
		{
			for (const UItemInstance* Item : Slots)
			{
				if (!IsValid(Item)
					|| Item->GetDefinition().GetObject() != &Definition)
				{
					continue;
				}
				const UItemStackFeature* Stack =
					Item->FindFeature<UItemStackFeature>();
				Quantity += Stack ? Stack->GetQuantity() : 1;
			}
		};
		CountSlots(Inventory.GetQuickbarSlots());
		CountSlots(Inventory.GetBackpackSlots());
		return Quantity;
	}

	AStickEquippedItemActor* CreateDormantStick(
		FGameplayWorld& Harness,
		UWorldObjectDefinition& Definition,
		const FVector& Location,
		FWorldObjectEntityHandle& OutEntity)
	{
		AStickEquippedItemActor* Actor =
			Harness.World->SpawnActor<AStickEquippedItemActor>();
		Actor->SetActorLocation(Location);
		FWorldObjectCreateDesc Desc;
		Desc.Definition = &Definition;
		Desc.WorldTransform = Actor->GetActorTransform();
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		Desc.Proxy = Actor->GetWorldObjectProxyComponent();
		OutEntity = Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>()
			->CreateEntity(Desc);
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStickWorldObjectLifecycleTest,
	"ElementSandbox.WorldObjects.Gameplay.StickEquipThrowSleepWakePickup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStickWorldObjectLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::GameplayTests;
	FGameplayWorld Harness;
	UInventoryComponent* Inventory = Harness.PlayerState
		? Harness.PlayerState->GetInventoryComponent()
		: nullptr;
	UEquipmentComponent* Equipment = Harness.Character
		? Harness.Character->GetEquipmentComponent()
		: nullptr;
	UWorldObjectEquipmentBridgeComponent* Bridge = Harness.Character
		? Harness.Character->GetWorldObjectEquipmentBridgeComponent()
		: nullptr;
	UWorldObjectWorldSubsystem* WorldObjects = Harness.World
		? Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	UWorldObjectItemCatalogSubsystem* Catalog = Harness.World
		? Harness.World->GetSubsystem<UWorldObjectItemCatalogSubsystem>()
		: nullptr;
	UItemDefinition* StickItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Stick.DA_Stick"));
	TestNotNull(TEXT("测试 Inventory"), Inventory);
	TestNotNull(TEXT("测试 Equipment"), Equipment);
	TestNotNull(TEXT("Items→WorldObject Bridge"), Bridge);
	TestNotNull(TEXT("WorldObject Subsystem"), WorldObjects);
	TestTrue(TEXT("木棍双向 Catalog 就绪"), Catalog && Catalog->IsReady());
	TestNotNull(TEXT("加载 DA_Stick"), StickItem);
	if (!Inventory || !Equipment || !Bridge || !WorldObjects
		|| !Catalog || !Catalog->IsReady() || !StickItem)
	{
		return false;
	}

	TestTrue(TEXT("木棍进入快捷栏"), Inventory->GrantItemToQuickbar(StickItem, 0));
	Inventory->SelectQuickbarSlot(0);
	AStickEquippedItemActor* StickActor =
		Cast<AStickEquippedItemActor>(Equipment->GetEquippedActor());
	UWorldObjectProxyComponent* Proxy = IsValid(StickActor)
		? StickActor->GetWorldObjectProxyComponent()
		: nullptr;
	const FWorldObjectEntityHandle OriginalEntity = Proxy
		? Proxy->GetLocalEntity()
		: FWorldObjectEntityHandle();
	const FWorldEntityId OriginalWorldEntityId = WorldObjects->GetWorldEntityId(OriginalEntity);
	TestNotNull(TEXT("装备生成木棍 Actor"), StickActor);
	TestTrue(TEXT("装备创建 Portable + Attached Entity"), OriginalEntity.IsSet());
	const FWorldObjectMotionFragment* Motion =
		WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(OriginalEntity);
	TestTrue(TEXT("装备状态是 Attached"),
		Motion && Motion->State == EWorldObjectMotionState::Attached);
	TestEqual(TEXT("Attached 进入 Active Array"),
		WorldObjects->GetRuntimeStats().ActiveCount, 1);
	Harness.Controller->RequestPickupWorldObject(OriginalWorldEntityId);
	TestTrue(TEXT("不能拾取正在持有的 Attached 木棍"), WorldObjects->IsEntityAlive(OriginalEntity));

	TestTrue(TEXT("服务器投掷当前木棍"), Bridge->ThrowSelectedStick(
		*Inventory,
		FVector::ForwardVector,
		1200.0,
		150.0));
	TestNull(TEXT("投掷原子移除当前快捷栏实例"),
		Inventory->GetItem(FInventorySlotAddress(EInventoryContainer::Quickbar, 0)));
	TestNull(TEXT("投掷撤销 Equipment 对 Actor 的所有权"),
		Equipment->GetEquippedActor());
	TestTrue(TEXT("投掷保留同一 Actor"), IsValid(StickActor));
	TestTrue(TEXT("投掷保留同一 Entity Handle"),
		Proxy && Proxy->GetLocalEntity() == OriginalEntity);
	TestTrue(TEXT("投掷保留同一 WorldEntityId"), WorldObjects->GetWorldEntityId(OriginalEntity) == OriginalWorldEntityId);
	TestTrue(TEXT("Capsule 开启服务器 Chaos"),
		Proxy && Proxy->GetPhysicsPrimitive()
		&& Proxy->GetPhysicsPrimitive()->IsSimulatingPhysics());
	Motion = WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(OriginalEntity);
	TestTrue(TEXT("投掷状态切换为 Physics"),
		Motion && Motion->State == EWorldObjectMotionState::Physics);

	WorldObjects->QueueProxyMotionState(OriginalWorldEntityId, EWorldObjectMotionState::Dormant);
	WorldObjects->QueueProxyMotionState(OriginalWorldEntityId, EWorldObjectMotionState::Dormant);
	AdvancePostActorFrame(*Harness.World);
	Motion = WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(OriginalEntity);
	TestTrue(TEXT("Chaos Sleep 后成为 Dormant"),
		Motion && Motion->State == EWorldObjectMotionState::Dormant);
	TestEqual(TEXT("重复 Sleep 只退出 Active 一次"),
			WorldObjects->GetRuntimeStats().ActiveCount, 0);
	TestTrue(TEXT("Dormant 自定义木棍保留表现 Actor 但关闭物理投影"),
			IsValid(StickActor) && Proxy && !Proxy->IsPhysicsProjectionActive() && Proxy->GetPhysicsPrimitive() &&
				!Proxy->GetPhysicsPrimitive()->IsSimulatingPhysics() &&
				Proxy->GetPhysicsPrimitive()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("Dormant 木棍仍留在 Dynamic Tree"),
			WorldObjects->GetSpatialIndex().Contains(OriginalEntity));

	TestTrue(TEXT("显式激活重启自定义木棍物理投影"),
		WorldObjects->ActivatePhysics(OriginalEntity, FVector(300.0, 0.0, 100.0)));
	TestEqual(TEXT("再次激活只加入 Active 一次"),
			WorldObjects->GetRuntimeStats().ActiveCount, 1);
	TestTrue(TEXT("再次激活恢复自定义 Actor 碰撞与模拟"),
			Proxy && Proxy->IsPhysicsProjectionActive() && Proxy->GetPhysicsPrimitive() &&
				Proxy->GetPhysicsPrimitive()->IsSimulatingPhysics() &&
				Proxy->GetPhysicsPrimitive()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
	TestTrue(TEXT("Wake 后仍是原 Handle"), WorldObjects->FindEntity(OriginalWorldEntityId) == OriginalEntity);

	WorldObjects->QueueProxyMotionState(OriginalWorldEntityId, EWorldObjectMotionState::Dormant);
	AdvancePostActorFrame(*Harness.World);
	const int32 ItemsBeforePickup = CountInventoryItems(*Inventory);
	Harness.Controller->RequestPickupWorldObject(OriginalWorldEntityId);
	TestFalse(TEXT("拾取销毁 WorldObject 身份"),
		WorldObjects->FindEntity(OriginalWorldEntityId).IsSet());
	TestFalse(TEXT("拾取销毁世界 Actor"), IsValid(StickActor));
	TestEqual(TEXT("拾取创建新的背包 ItemInstance"),
		CountInventoryItems(*Inventory), ItemsBeforePickup + 1);
	Harness.Controller->RequestPickupWorldObject(OriginalWorldEntityId);
	TestEqual(TEXT("重复拾取同一 WorldEntityId 不会重复发物品"),
		CountInventoryItems(*Inventory), ItemsBeforePickup + 1);
	TestFalse(TEXT("没有当前装备时投掷被拒绝"), Bridge->ThrowSelectedStick(
		*Inventory,
		FVector::ForwardVector,
		1200.0,
		150.0));

	UWorldObjectDefinition* StickWorldDefinition =
		Catalog->FindWorldObjectDefinition(StickItem);
	FWorldObjectEntityHandle FarEntity;
	CreateDormantStick(
		Harness,
		*StickWorldDefinition,
		Harness.Character->GetActorLocation() + FVector(1000.0, 0.0, 0.0),
		FarEntity);
	const FWorldEntityId FarWorldEntityId = WorldObjects->GetWorldEntityId(FarEntity);
	Harness.Controller->RequestPickupWorldObject(FarWorldEntityId);
	TestTrue(TEXT("超过 300cm 的拾取被服务器拒绝"),
		WorldObjects->IsEntityAlive(FarEntity));
	WorldObjects->DestroyEntity(FarEntity);

	for (int32 Attempt = 0; Attempt < 256 && Inventory->CanAddItem(StickItem, 1); ++Attempt)
	{
		Inventory->AddItem(StickItem, 1, EInventoryContainer::Backpack);
	}
	TestFalse(TEXT("测试背包与快捷栏已无容量"), Inventory->CanAddItem(StickItem, 1));
	FWorldObjectEntityHandle FullInventoryEntity;
	CreateDormantStick(
		Harness,
		*StickWorldDefinition,
		Harness.Character->GetActorLocation() + FVector(100.0, 0.0, 0.0),
		FullInventoryEntity);
	const FWorldEntityId FullInventoryWorldEntityId =
		WorldObjects->GetWorldEntityId(FullInventoryEntity);
	Harness.Controller->RequestPickupWorldObject(FullInventoryWorldEntityId);
	TestTrue(TEXT("背包满时拾取被服务器拒绝"),
		WorldObjects->IsEntityAlive(FullInventoryEntity));
	TestTrue(TEXT("清出一个背包槽"), Inventory->RemoveItem(
		FInventorySlotAddress(EInventoryContainer::Backpack, 0)));
	Harness.Controller->RequestPickupWorldObject(FullInventoryWorldEntityId);
	TestFalse(TEXT("有容量后同一 WorldEntityId 可以拾取"),
		WorldObjects->IsEntityAlive(FullInventoryEntity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectCatalogPickupResolutionTest,
	"ElementSandbox.WorldObjects.Gameplay.CatalogPickupResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectCatalogPickupResolutionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::GameplayTests;
	FGameplayWorld Harness;
	UWorldObjectWorldSubsystem* WorldObjects =
		Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>();
	UWorldObjectItemCatalogSubsystem* Catalog =
		Harness.World->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
	if (!TestTrue(TEXT("Catalog 拾取测试环境有效"),
		WorldObjects && Catalog && Catalog->IsReady()))
	{
		return false;
	}

	UItemDefinition* StickItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Stick.DA_Stick"));
	UWorldObjectDefinition* StickDefinition =
		Catalog->FindWorldObjectDefinition(StickItem);
	if (!TestTrue(TEXT("木棍 Catalog 回退定义有效"),
		StickItem && StickDefinition))
	{
		return false;
	}
	FWorldObjectEntityHandle StickEntity;
	CreateDormantStick(
		Harness,
		*StickDefinition,
		Harness.Character->GetActorLocation() + FVector(80.0, 0.0, 0.0),
		StickEntity);
	UE::ElementSandbox::FWorldObjectPickupResolution Resolution;
	TestTrue(TEXT("木棍拾取数据唯一来自 Definition Catalog"),
		UE::ElementSandbox::TryResolveWorldObjectPickup(
			*WorldObjects,
			StickEntity,
			*Catalog,
			Resolution)
		&& Resolution.ItemDefinition == StickItem
		&& Resolution.Quantity == 1);
	WorldObjects->DestroyEntity(StickEntity);

	UItemDefinition* CharcoalItem = LoadObject<UItemDefinition>(
		nullptr,
		TEXT("/Game/Items/DA_Charcoal.DA_Charcoal"));
	UWorldObjectDefinition* CharcoalDefinition =
		Catalog->FindWorldObjectDefinition(CharcoalItem);
	if (!TestTrue(TEXT("木炭 Item 与 WorldObject 双向映射有效"),
		CharcoalItem
			&& CharcoalDefinition == GetDefault<UCharcoalWorldObjectDefinition>()
			&& Catalog->FindItemDefinition(CharcoalDefinition) == CharcoalItem))
	{
		return false;
	}

	FWorldObjectCreateDesc CharcoalDesc;
	CharcoalDesc.Definition = CharcoalDefinition;
	CharcoalDesc.WorldTransform.SetLocation(
		Harness.Character->GetActorLocation() + FVector(80.0, 0.0, 0.0));
	const FWorldObjectEntityHandle CharcoalEntity =
		WorldObjects->CreateEntity(CharcoalDesc);
	const FWorldEntityId CharcoalWorldEntityId =
		WorldObjects->GetWorldEntityId(CharcoalEntity);
	TestTrue(TEXT("Dormant 木炭可解析为一个木炭道具"),
		CharcoalEntity.IsSet()
			&& UE::ElementSandbox::TryResolveWorldObjectPickup(
				*WorldObjects,
				CharcoalEntity,
				*Catalog,
				Resolution)
			&& Resolution.ItemDefinition == CharcoalItem
			&& Resolution.Quantity == 1);

	UInventoryComponent* Inventory = Harness.PlayerState->GetInventoryComponent();
	const int32 CharcoalBeforePickup = Inventory
		? CountInventoryQuantity(*Inventory, *CharcoalItem)
		: 0;
	Harness.Controller->RequestPickupWorldObject(CharcoalWorldEntityId);
	TestFalse(TEXT("E 交互共用的权威拾取请求会结束木炭 WorldObject"),
		WorldObjects->IsEntityAlive(CharcoalEntity));
	TestTrue(TEXT("拾取后背包增加一个木炭"),
		Inventory
			&& CountInventoryQuantity(*Inventory, *CharcoalItem)
				== CharcoalBeforePickup + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectPickupSurfaceReachTest,
	"ElementSandbox.WorldObjects.Gameplay.PickupSurfaceReachMatchesFocus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectPickupSurfaceReachTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::GameplayTests;
	FGameplayWorld Harness;
	// 先注册 Focus 查询，再接入背包；此规则测试没有 LocalPlayer，不创建 HUD。
	Harness.Controller->SetPlayerState(nullptr);
	Harness.Controller->DispatchBeginPlay();
	Harness.Controller->SetPlayerState(Harness.PlayerState);
	auto* WorldObjects = Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>();
	auto* Catalog = Harness.World->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
	auto* Inventory = Harness.PlayerState->GetInventoryComponent();
	auto* Focus = Harness.Controller->FindComponentByClass<UFocusHostComponent>();
	auto* Definition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
	if (!TestTrue(TEXT("木块拾取与 Focus 环境就绪"),
		WorldObjects && Catalog && Inventory && Focus && WorldObjects->RegisterDefinition(*Definition)))
	{
		return false;
	}
	UItemDefinition* Item = Catalog->FindItemDefinition(Definition);
	if (!TestNotNull(TEXT("木块可解析为背包物品"), Item)) return false;

	const FVector CharacterLocation = Harness.Character->GetActorLocation();
	const double Reach = Harness.Character->GetFocusDistance();
	FFocusQueryContext Context;
	Context.ViewOrigin = CharacterLocation;
	Context.ViewDirection = FVector::ForwardVector;
	for (const double Scale : {1.0, 2.0})
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Definition;
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		Desc.WorldTransform = FTransform(FQuat::Identity,
			CharacterLocation + FVector(Reach + 35.0 * Scale, 0.0, 0.0), FVector(Scale));
		const auto Entity = WorldObjects->CreateEntity(Desc);
		const FWorldEntityId Id = WorldObjects->GetWorldEntityId(Entity);
		if (!TestTrue(TEXT("创建中心超过拾取距离、表面仍可触及的木块"), Id.IsSet())) return false;
		Focus->EvaluateFocus(Context);
		const FFocusQueryHit* Hit = Focus->GetFocusedHit();
		const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
		TestTrue(TEXT("客户端 Focus 可以选中木块表面"), Target && Target->WorldEntityId == Id);
		const int32 QuantityBefore = CountInventoryQuantity(*Inventory, *Item);
		Harness.Controller->RequestPickupWorldObject(Id);
		TestFalse(TEXT("Authority 接受同一个可触及木块"), WorldObjects->FindEntity(Id).IsSet());
		TestEqual(TEXT("拾取增加一份物品"), CountInventoryQuantity(*Inventory, *Item), QuantityBefore + 1);
		Harness.Controller->RequestPickupWorldObject(Id);
		TestEqual(TEXT("重复请求不重复发物品"), CountInventoryQuantity(*Inventory, *Item), QuantityBefore + 1);
	}

	FWorldObjectCreateDesc FarDesc;
	FarDesc.Definition = Definition;
	FarDesc.MotionState = EWorldObjectMotionState::Dormant;
	FarDesc.WorldTransform.SetLocation(CharacterLocation + FVector(Reach + 95.0, 0.0, 0.0));
	const auto FarEntity = WorldObjects->CreateEntity(FarDesc);
	Focus->EvaluateFocus(Context);
	TestNull(TEXT("表面也超出范围时不显示拾取目标"), Focus->GetFocusedHit());
	Harness.Controller->RequestPickupWorldObject(WorldObjects->GetWorldEntityId(FarEntity));
	TestTrue(TEXT("Authority 仍拒绝超范围拾取"), WorldObjects->IsEntityAlive(FarEntity));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectRotatedPickupTest,
	"ElementSandbox.WorldObjects.Gameplay.RotatedPickupMatchesVisibleBox",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectRotatedPickupTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::WorldObjects::GameplayTests;
	FGameplayWorld Harness;
	Harness.Controller->SetPlayerState(nullptr);
	Harness.Controller->DispatchBeginPlay();
	Harness.Controller->SetPlayerState(Harness.PlayerState);
	auto* Objects = Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>();
	auto* Focus = Harness.Controller->FindComponentByClass<UFocusHostComponent>();
	auto* Definition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
	if (!TestTrue(TEXT("旋转木块拾取环境就绪"), Objects && Focus && Objects->RegisterDefinition(*Definition))) return false;
	const FVector Origin = Harness.Character->GetActorLocation();
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Definition;
	Desc.WorldTransform = FTransform(FRotator(0, 45, 0), Origin + FVector(180, 0, 0));
	const auto Rotated = Objects->CreateEntity(Desc);
	const auto RotatedId = Objects->GetWorldEntityId(Rotated);
	if (!TestTrue(TEXT("创建斜放木块"), RotatedId.IsSet())) return false;

	FFocusQueryContext Context;
	// 点位落在旋转后的世界 AABB 角落，距离真实长方体侧面仍超过半宽。
	Context.ViewOrigin = Origin + FVector(120, 60, 100);
	Context.ViewDirection = -FVector::UpVector;
	Focus->EvaluateFocus(Context);
	const auto* AssistedHit = Focus->GetFocusedHit();
	TestTrue(TEXT("AABB 空白角落只能作为附近辅助，不能伪装成直接命中"),
		AssistedHit && !AssistedHit->bDirectAim);
	Context.ViewOrigin = Origin + FVector(180, 0, 100);
	Focus->EvaluateFocus(Context);
	const auto* Hit = Focus->GetFocusedHit();
	const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
	TestTrue(TEXT("准星落在木块真实顶面时命中同一身份"), Target && Target->WorldEntityId == RotatedId);
	Objects->DestroyEntity(Rotated);

	// 大斜盒的 AABB 先相交，但小盒的实际表面更近。必须比较窄相距离。
	Desc.WorldTransform = FTransform(FRotator(0, 45, 0), Origin + FVector(120, 60, 0));
	const auto Behind = Objects->CreateEntity(Desc);
	Desc.WorldTransform = FTransform(Origin + FVector(62, 0, 0));
	Desc.InstanceInteractionBounds = FBox(FVector(-4), FVector(4));
	const auto Front = Objects->CreateEntity(Desc);
	Context.ViewOrigin = Origin;
	Context.ViewDirection = FVector::ForwardVector;
	Focus->EvaluateFocus(Context);
	Hit = Focus->GetFocusedHit();
	Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
	TestTrue(TEXT("粗筛顺序不能让斜放木块抢走前方物件的 E"), Target && Target->WorldEntityId == Objects->GetWorldEntityId(Front));
	TestTrue(TEXT("命中点位于最近物件表面"), Hit && Hit->HitLocation.Equals(Origin + FVector(58, 0, 0), 0.01));
	Objects->DestroyEntity(Behind);
	Objects->DestroyEntity(Front);

	// AABB 最近角落在 300cm 内，OBB 的真实最近点在 300cm 外，服务器也必须拒绝。
	const double Reach = Harness.Character->GetFocusDistance();
	Desc.InstanceInteractionBounds.Reset();
	Desc.WorldTransform = FTransform(FRotator(0, 45, 0), Origin + FVector(Reach + 55, -60, 0));
	const auto Far = Objects->CreateEntity(Desc);
	Harness.Controller->RequestPickupWorldObject(Objects->GetWorldEntityId(Far));
	TestTrue(TEXT("Authority 按旋转木块的真实表面距离拒绝超范围拾取"), Objects->IsEntityAlive(Far));
	Objects->DestroyEntity(Far);
	return true;
}

#endif
