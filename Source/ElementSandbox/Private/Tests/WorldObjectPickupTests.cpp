#if WITH_DEV_AUTOMATION_TESTS

#include "BuildingWorldSubsystem.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Collision/BuildCollisionHost.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tests/FocusTestTypes.h"
#include "Wood/WoodBuildingDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupAuthorityService.h"
#include "WorldObjects/WorldObjectPickupComponent.h"
#include "WorldObjects/WorldObjectPickupResolver.h"

namespace
{
	struct FPickupWorld final
	{
		FPickupWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("PickupInteractionTest"), nullptr, true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			State = World->SpawnActor<AElementSandboxPlayerState>();
			Controller = World->SpawnActor<AElementSandboxPlayerController>();
			Pawn = World->SpawnActor<AElementSandboxCharacter>();
			Pawn->SetActorLocation(FVector(0, 0, 100));
			Controller->SetPlayerState(State);
			Controller->Possess(Pawn);
			Pawn->DispatchBeginPlay();
			// 自动化 World 没有 LocalPlayer，只启用查询与输入，不创建背包 Widget。
			Controller->SetPlayerState(nullptr);
			Controller->DispatchBeginPlay();
			Controller->SetPlayerState(State);
			Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			Catalog = World->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
			Inventory = State->GetInventoryComponent();
			Focus = Controller->FindComponentByClass<UFocusHostComponent>();
			Input = Controller->FindComponentByClass<UWorldObjectPickupComponent>();
			View.ViewOrigin = FVector(-400, 0, 220);
			View.ViewDirection = FVector::ForwardVector;
		}
		~FPickupWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		FWorldEntityId Wood(const FVector& Location, const EWorldObjectMotionState Motion = EWorldObjectMotionState::Dormant)
		{
			FWorldObjectCreateDesc Desc;
			Desc.Definition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
			Desc.WorldTransform.SetLocation(Location);
			Desc.MotionState = Motion;
			if (Motion == EWorldObjectMotionState::Physics)
			{
				Desc.InstanceInteractionBounds = Desc.Definition->InteractionLocalBounds;
				FWorldObjectPhysicsBodyInit Body;
				Body.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
				Desc.PhysicsBody = Body;
			}
			return Objects->GetWorldEntityId(Objects->CreateEntity(Desc));
		}
		FWorldEntityId Select()
		{
			Focus->EvaluateFocus(View);
			const auto* Hit = Focus->GetFocusedHit();
			const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
			return Target ? Target->WorldEntityId : FWorldEntityId();
		}
		int32 CountWood() const
		{
			const auto* Definition = Catalog->FindItemDefinition(GetDefault<UWoodBlockWorldObjectDefinition>());
			int32 Total = 0;
			for (const UItemInstance* Item : Inventory->GetBackpackSlots())
				if (Item && Item->GetDefinition().GetObject() == Definition)
					if (const auto* Stack = Item->FindFeature<UItemStackFeature>()) Total += Stack->GetQuantity();
			return Total;
		}
		EWorldObjectPickupFailure Pick(const FWorldEntityId Id)
		{
			return FWorldObjectPickupAuthorityService::TryPickup(*Objects, *Catalog, *Inventory,
				*Pawn, Id, Pawn->GetFocusDistance());
		}
		UWorld* World;
		AElementSandboxPlayerState* State;
		AElementSandboxPlayerController* Controller;
		AElementSandboxCharacter* Pawn;
		UWorldObjectWorldSubsystem* Objects;
		UWorldObjectItemCatalogSubsystem* Catalog;
		UInventoryComponent* Inventory;
		UFocusHostComponent* Focus;
		UWorldObjectPickupComponent* Input;
		FFocusQueryContext View;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNearbyPickupSelectionTest,
	"ElementSandbox.WorldObjects.Pickup.NearbyFeetDirectAimAndRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNearbyPickupSelectionTest::RunTest(const FString& Parameters)
{
	FPickupWorld H;
	const auto Left = H.Wood(FVector(190, 65, 20));
	const auto Right = H.Wood(FVector(195, -65, 20));
	if (!TestTrue(TEXT("附近夹具有效"), Left.IsSet() && Right.IsSet())) return false;
	TestTrue(TEXT("正常平视时可以选择地上的木块"), H.Select() == Left);
	TestTrue(TEXT("平视辅助不伪装成准星命中"), H.Focus->GetFocusedHit() && !H.Focus->GetFocusedHit()->bDirectAim);
	H.View.ViewDirection = FRotator(0, -3, 0).Vector();
	TestTrue(TEXT("小幅转镜头后保持当前辅助目标"), H.Select() == Left);
	H.View.ViewDirection = (FVector(195, -65, 20) - H.View.ViewOrigin).GetSafeNormal();
	TestTrue(TEXT("明确瞄准另一块时立即切换"), H.Select() == Right);
	TestTrue(TEXT("直接命中保留真实射线语义"), H.Focus->GetFocusedHit() && H.Focus->GetFocusedHit()->bDirectAim);
	H.Objects->DestroyEntity(H.Objects->FindEntity(Left));
	H.Objects->DestroyEntity(H.Objects->FindEntity(Right));
	const auto Feet = H.Wood(FVector(-50, 30, 20));
	H.View.ViewDirection = FVector::ForwardVector;
	TestTrue(TEXT("脚边物件不要求压低相机或转身"), H.Select() == Feet);
	H.Objects->DestroyEntity(H.Objects->FindEntity(Feet));
	const auto Behind = H.Wood(FVector(-240, 0, 20));
	TestTrue(TEXT("脚边范围外的身后物件不抢焦点"), Behind.IsSet() && !H.Select().IsSet());
	H.Pawn->SetActorLocation(FVector(2000, 0, 100));
	TestFalse(TEXT("超距物件立即失焦"), H.Select().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPickupObstructionTest,
	"ElementSandbox.WorldObjects.Pickup.ObstructionWithoutBuildingProxy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPickupObstructionTest::RunTest(const FString& Parameters)
{
	FPickupWorld H;
	const auto Id = H.Wood(FVector(230, 0, 20));
	auto* Buildings = H.World->GetSubsystem<UBuildingWorldSubsystem>();
	auto* Wall = NewObject<UWoodBuildingDefinition>(Buildings);
	if (!TestTrue(TEXT("构造挡在物件前面的逻辑墙体"), Id.IsSet() && Wall->Initialize(
		TEXT("Test.Pickup.Obstruction"), FVector(20, 400, 400)) && Buildings->RegisterDefinition(*Wall))) return false;
	const auto WallEntity = Buildings->CreateEntity(*Wall, FTransform(FVector(100, 0, 0)));
	if (!TestTrue(TEXT("墙体已注册"), WallEntity.IsSet())) return false;
	if (auto* Host = Buildings->GetCollisionHost()) Host->SetActorEnableCollision(false);
	TestFalse(TEXT("没有碰撞代理的 Building 仍遮挡客户端拾取"), H.Select().IsSet());
	TestEqual(TEXT("服务器也按 ECS 墙体拒绝"), H.Pick(Id), EWorldObjectPickupFailure::Obstructed);
	TestEqual(TEXT("遮挡拒绝不增加物品"), H.CountWood(), 0);
	TestTrue(TEXT("遮挡拒绝保留物件"), H.Objects->FindEntity(Id).IsSet());
	Buildings->DestroyEntity(WallEntity);
	TestTrue(TEXT("移走墙体后立即恢复附近选物"), H.Select() == Id);

	AActor* Obstacle = H.World->SpawnActor<AActor>();
	auto* Box = NewObject<UBoxComponent>(Obstacle);
	Obstacle->SetRootComponent(Box);
	Box->SetBoxExtent(FVector(10, 200, 200));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	Obstacle->SetActorLocation(FVector(100, 0, 150));
	TestEqual(TEXT("普通 UE 世界障碍也由服务器拒绝"), H.Pick(Id), EWorldObjectPickupFailure::Obstructed);
	Obstacle->Destroy();
	TestEqual(TEXT("解除全部遮挡后可以拾取"), H.Pick(Id), EWorldObjectPickupFailure::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMovingPickupTransactionTest,
	"ElementSandbox.WorldObjects.Pickup.MovingPoseAndAtomicRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMovingPickupTransactionTest::RunTest(const FString& Parameters)
{
	FPickupWorld H;
	const auto Id = H.Wood(FVector(220, 0, 80), EWorldObjectMotionState::Physics);
	const auto Entity = H.Objects->FindEntity(Id);
	const auto* Proxy = H.Objects->GetProxy(Entity);
	auto* Actor = Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
	if (!TestNotNull(TEXT("运动物件具有真实 Physics 代理"), Actor)) return false;
	Actor->ReleasePhysicsImmediately();
	Actor->GetPhysicsBox()->SetPhysicsLinearVelocity(FVector(30, 0, 0));
	TestTrue(TEXT("Physics 状态保持附近拾取资格"), H.Select() == Id);
	Actor->SetActorLocation(FVector(2000, 0, 80), false, nullptr, ETeleportType::TeleportPhysics);
	TestEqual(TEXT("ECS 还停留在旧位姿时，服务器以当前物理位置拒绝超距"),
		H.Pick(Id), EWorldObjectPickupFailure::OutOfRange);
	Actor->SetActorLocation(FVector(220, 0, 80), false, nullptr, ETeleportType::TeleportPhysics);
	const auto Veto = H.Objects->OnEntityPreDestroy().AddLambda(
		[Entity](const FWorldObjectEntityHandle Target, bool& bCanDestroy) { if (Target == Entity) bCanDestroy = false; });
	TestEqual(TEXT("宿主否决返回明确失败"), H.Pick(Id), EWorldObjectPickupFailure::DestroyRejected);
	TestEqual(TEXT("否决完整回滚预留的背包增加"), H.CountWood(), 0);
	TestTrue(TEXT("否决后物件和活动代理仍存在"), H.Objects->IsEntityAlive(Entity) && IsValid(Actor));
	H.Objects->OnEntityPreDestroy().Remove(Veto);
	TestEqual(TEXT("运动中也能完成拾取"), H.Pick(Id), EWorldObjectPickupFailure::None);
	TestEqual(TEXT("只增加一份木块"), H.CountWood(), 1);
	TestFalse(TEXT("成功会结束物理代理"), IsValid(Actor));
	TestFalse(TEXT("成功会结束世界身份"), H.Objects->FindEntity(Id).IsSet());
	TestEqual(TEXT("重复权威请求拒绝已经消费的身份"), H.Pick(Id), EWorldObjectPickupFailure::InvalidTarget);
	TestEqual(TEXT("重复请求没有额外奖励"), H.CountWood(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPickupHoldInputTest,
	"ElementSandbox.WorldObjects.Pickup.HoldReleaseAndDiscreteInteractionIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPickupHoldInputTest::RunTest(const FString& Parameters)
{
	FPickupWorld H;
	H.Wood(FVector(150, 40, 20));
	H.Wood(FVector(200, -50, 20));
	H.Wood(FVector(260, 40, 20));
	if (!TestTrue(TEXT("开始前有附近目标"), H.Select().IsSet())) return false;
	TestTrue(TEXT("按下执行第一件拾取"), H.Input->BeginInteract());
	TestEqual(TEXT("按下只拾取一件"), H.CountWood(), 1);
	TestTrue(TEXT("拾取按下进入持续收集"), H.Input->IsCollecting());
	H.Select();
	H.Input->AdvanceCollection(0.20);
	TestEqual(TEXT("短按不会提前重复"), H.CountWood(), 1);

	auto* DoorHandler = NewObject<UFocusTestHandler>(H.Controller);
	H.Focus->RegisterQuery(*DoorHandler, FFocusQueryDelegate::CreateLambda(
		[](const FFocusQueryContext&, TArray<FFocusQueryHit>& Hits)
		{
			FFocusTestTarget Target; Target.Value = 99;
			FFocusQueryHit& Hit = Hits.AddDefaulted_GetRef();
			Hit.HitDistance = 1.0;
			Hit.Target = FInstancedStruct::Make<FFocusTestTarget>(Target);
		}), *DoorHandler);
	TestTrue(TEXT("持续收集时直接瞄准的离散操作不抢走拾取"), H.Select().IsSet());
	H.Input->AdvanceCollection(0.35);
	TestEqual(TEXT("长按到期只拾取下一件"), H.CountWood(), 2);
	TestEqual(TEXT("持续收集从未触发门操作"), DoorHandler->InteractCount, 0);
	H.Input->EndInteract();
	H.Select();
	H.Input->AdvanceCollection(2.0);
	TestEqual(TEXT("释放后停止收集"), H.CountWood(), 2);
	TestTrue(TEXT("释放后门可以接收一次离散按下"), H.Input->BeginInteract());
	TestFalse(TEXT("门操作不进入持续收集"), H.Input->IsCollecting());
	H.Input->AdvanceCollection(3.0);
	TestEqual(TEXT("门只执行一次"), DoorHandler->InteractCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPickupPendingRequestTest,
	"ElementSandbox.WorldObjects.Pickup.PendingAcknowledgementAndTombstone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPickupPendingRequestTest::RunTest(const FString& Parameters)
{
	FPickupWorld H;
	const auto First = H.Wood(FVector(150, 40, 20));
	const auto Second = H.Wood(FVector(230, -50, 20));
	TestTrue(TEXT("取得第一个在途请求"), H.Input->TryBeginRequest(First));
	TestFalse(TEXT("同一身份等待期间拒绝重发"), H.Input->TryBeginRequest(First));
	TestFalse(TEXT("确认到达前不积累第二个 RPC"), H.Input->TryBeginRequest(Second));
	TestTrue(TEXT("等待中的目标不会重新成为提示"), H.Select() == Second);
	H.Input->CompleteRequest(First, EWorldObjectPickupFailure::None);
	TestTrue(TEXT("ACK 不会在客户端提前删 ECS"), H.Objects->FindEntity(First).IsSet());
	TestTrue(TEXT("ACK 先于 Tombstone 时继续排除同一目标"), H.Input->IsTargetUnavailable(First));
	TestFalse(TEXT("ACK 后同一目标仍不允许重复请求"), H.Input->TryBeginRequest(First));
	TestTrue(TEXT("可以继续处理其他目标"), H.Input->TryBeginRequest(Second));
	H.Input->CompleteRequest(Second, EWorldObjectPickupFailure::InventoryFull);
	FText Feedback;
	TestTrue(TEXT("背包满有明确反馈"), H.Input->TryGetFeedback(Feedback) && Feedback.ToString().Contains(TEXT("背包已满")));
	H.Objects->DestroyEntity(H.Objects->FindEntity(First));
	H.Input->AdvanceCollection(1.0);
	TestFalse(TEXT("正常 Tombstone 到达后释放临时身份集合"), H.Input->IsTargetUnavailable(First));
	H.Input->BeginInteract();
	H.Controller->UnPossess();
	TestFalse(TEXT("Pawn 生命周期结束立即释放持续输入"), H.Input->IsCollecting());
	return true;
}

#endif
