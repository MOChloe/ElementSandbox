#if WITH_DEV_AUTOMATION_TESTS

#include "Chunk/WorldChunkTypes.h"
#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/DemoNetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tests/WorldObjectTestPhysicsDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"

namespace
{
	struct FProjectionTestWorld final
	{
		explicit FProjectionTestWorld(const FName Name, const bool bClient = false)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			if (bClient)
			{
				// 内存连接只提供真实 NM_Client 上下文；测试手动排列正式 Record/Actor 的到达次序。
				Driver = NewObject<UDemoNetDriver>(World);
				Driver->ServerConnection = NewObject<UDemoNetConnection>(Driver);
				World->SetNetDriver(Driver);
			}
			Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			Definition = NewObject<UWorldObjectTestPhysicsDefinition>(World);
			Definition->DefinitionId = TEXT("Test.PredictedPhysicsHandoff");
			Objects->RegisterDefinition(*Definition);
		}
		~FProjectionTestWorld()
		{
			World->SetNetDriver(nullptr);
			if (Driver) Driver->ServerConnection = nullptr;
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}
		UWorld* World = nullptr;
		UDemoNetDriver* Driver = nullptr;
		UWorldObjectWorldSubsystem* Objects = nullptr;
		UWorldObjectTestPhysicsDefinition* Definition = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldObjectClientPhysicsHandoffTest,
	"ElementSandbox.WorldObjects.Network.ClientPhysicsHandoffOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldObjectClientPhysicsHandoffTest::RunTest(const FString& Parameters)
{
	FProjectionTestWorld Authority(TEXT("PhysicsHandoffAuthority"));
	FWorldObjectCreateDesc Desc;
	Desc.Definition = Authority.Definition;
	Desc.WorldTransform = FTransform(FVector(150.0, 0.0, 80.0));
	Desc.MotionState = EWorldObjectMotionState::Physics;
	Desc.InstanceInteractionBounds = Desc.Definition->InteractionLocalBounds;
	FWorldObjectPhysicsBodyInit Body;
	Body.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
	Desc.PhysicsBody = Body;
	const auto AuthorityEntity = Authority.Objects->CreateEntity(Desc);
	const FWorldEntityId Id = Authority.Objects->GetWorldEntityId(AuthorityEntity);
	if (!TestTrue(TEXT("创建权威物件"), Id.IsSet())) return false;
	const FWorldChunkCoord Chunk = FWorldChunkCoord::FromWorldLocation(Desc.WorldTransform.GetLocation());
	FString Error;
	TArray<FWorldPersistentEntityRecord> Dormant, Awake, FinalDormant, NextAwake;
	const auto Capture = [&](TArray<FWorldPersistentEntityRecord>& Records)
	{
		return Authority.Objects->CapturePersistentBatchForTesting(MakeArrayView(&Id, 1), Records, Error);
	};
	Authority.Objects->SetMotionState(AuthorityEntity, EWorldObjectMotionState::Dormant);
	if (!TestTrue(TEXT("捕获静止 Record"), Capture(Dormant))) return false;
	Authority.Objects->ActivatePhysics(AuthorityEntity, FVector::ZeroVector);
	if (!TestTrue(TEXT("捕获第一次唤醒 Record"), Capture(Awake))) return false;
	Authority.Objects->SetMotionState(AuthorityEntity, EWorldObjectMotionState::Dormant);
	if (!TestTrue(TEXT("捕获最终静止 Record"), Capture(FinalDormant))) return false;
	Authority.Objects->ActivatePhysics(AuthorityEntity, FVector::ZeroVector);
	if (!TestTrue(TEXT("捕获第二次唤醒 Record"), Capture(NextAwake))) return false;

	FProjectionTestWorld Client(TEXT("PhysicsHandoffClient"), true);
	TestEqual(TEXT("以实际 Client 模式执行投影"), Client.World->GetNetMode(), NM_Client);
	const auto Restore = [&](const TArray<FWorldPersistentEntityRecord>& Records)
	{
		return Client.Objects->RestorePersistentBatchForTesting(Chunk, Records, Error);
	};
	if (!TestTrue(TEXT("客户端先收到静止 Record"), Restore(Dormant))) return false;
	auto Entity = Client.Objects->FindEntity(Id);
	auto* Collision = Client.World->GetSubsystem<UWorldObjectCollisionWorldSubsystem>();
	FWorldObjectCollisionSource Source;
	Source.ViewDirection = FVector::ForwardVector;
	Source.PawnContactBounds = FBox(FVector(-50.0), FVector(50.0));
	Source.ImmediateBounds = FBox(FVector(-400.0), FVector(400.0));
	Source.PrefetchBounds = Source.ImmediateBounds;
	Source.RetentionBounds = Source.ImmediateBounds;
	Source.Revision = 1;
	Collision->RegisterSource(Source);
	Collision->FlushImmediateCollisionChanges();
	TestEqual(TEXT("初始静止碰撞存在"), Collision->GetStats().CollisionInstanceCount, 1);

	const auto SpawnReplica = [&](const uint32 ActivationRevision)
	{
		auto* Actor = Client.World->SpawnActor<AWorldObjectPhysicsProxyActor>();
		Actor->SetRole(ROLE_Authority);
		Actor->SetActorTransform(Desc.WorldTransform);
		Actor->ConfigurePhysics(Desc.InstanceInteractionBounds.GetValue(), Body.MassKg,
			Body.CollisionPolicy, FVector::ZeroVector, FVector::ZeroVector, ActivationRevision);
		Actor->ReleasePhysicsImmediately();
		Actor->GetWorldObjectProxyComponent()->AssignAuthorityWorldEntityId(Id);
		Actor->SetRole(ROLE_SimulatedProxy);
		Actor->RefreshClientPhysicsProjection();
		Actor->DispatchBeginPlay();
		return Actor;
	};
	auto* First = SpawnReplica(Awake[0].StateRevision);
	TestTrue(TEXT("夹具初始化真实 Actor 生命周期"), First->IsActorInitialized() && First->HasActorBegunPlay());
	TestEqual(TEXT("LooseDebris 使用 UE 预测插值复制"), First->GetPhysicsReplicationMode(), EPhysicsReplicationMode::PredictiveInterpolation);
	TestFalse(TEXT("Actor 先到时不与旧静止碰撞重叠"), First->GetPhysicsBox()->IsSimulatingPhysics());
	TestEqual(TEXT("等待 Physics Record 时保留阻挡"), Collision->GetStats().CollisionInstanceCount, 1);
	TestTrue(TEXT("收到 Physics Record"), Restore(Awake));
	TestTrue(TEXT("客户端可预测刚体接触"), First->GetPhysicsBox()->IsSimulatingPhysics());
	TestEqual(TEXT("刚体接管即移除静止碰撞"), Collision->GetStats().CollisionInstanceCount, 0);
	const FVector PredictedPosition = Desc.WorldTransform.GetLocation() + FVector(0.0, -1.0, 0.0);
	First->SetActorLocation(PredictedPosition, false, nullptr, ETeleportType::TeleportPhysics);
	First->GetPhysicsBox()->PutAllRigidBodiesToSleep();
	++GFrameCounter;
	FWorldDelegates::OnWorldPostActorTick.Broadcast(Client.World, LEVELTICK_All, 1.0f / 60.0f);
	TestEqual(TEXT("客户端局部休眠不能提交权威 MotionState"),
		Client.Objects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity)->State, EWorldObjectMotionState::Physics);
	TestTrue(TEXT("客户端预测位姿允许越过 Chunk 边界"),
		Client.Objects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity)->WorldTransform.GetLocation().Equals(PredictedPosition));
	TestEqual(TEXT("客户端越界不增加权威 Revision"),
		Client.Objects->GetRegistry().FindFragment<FWorldObjectWorldIdentityFragment>(Entity)->StateRevision, Awake[0].StateRevision);
	TestTrue(TEXT("Dormant 先于 Actor 销毁到达"), Restore(FinalDormant));
	TestFalse(TEXT("收到 Dormant 立即关闭旧刚体"), First->GetPhysicsBox()->IsSimulatingPhysics());
	TestEqual(TEXT("Dormant 接管只有一份静止碰撞"), Collision->GetStats().CollisionInstanceCount, 1);
	FRepMovement LateMovement = First->GetReplicatedMovement();
	LateMovement.bRepPhysics = true;
	First->SetReplicatedMovement(LateMovement);
	First->OnRep_ReplicatedMovement();
	TestEqual(TEXT("迟到 Movement 不重新开启旧碰撞"), First->GetPhysicsBox()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

	auto* Second = SpawnReplica(NextAwake[0].StateRevision);
	TestTrue(TEXT("新一轮 Actor 能在旧 Actor EndPlay 前按 Revision 接管"),
		Client.Objects->GetProxy(Entity) == Second->GetWorldObjectProxyComponent());
	TestTrue(TEXT("第二次 Physics Record 到达"), Restore(NextAwake));
	// 复制销毁由服务器授权；普通 Client Destroy() 会被 UE 按 Role 拒绝。
	TestTrue(TEXT("接收旧 Actor 的复制销毁"), First->Destroy(true));
	TestTrue(TEXT("旧 Actor 销毁不清除新绑定"), Client.Objects->GetProxy(Entity) == Second->GetWorldObjectProxyComponent());
	TestTrue(TEXT("新代理继续预测"), Second->GetPhysicsBox()->IsSimulatingPhysics());
	TArray<FWorldPersistentEntityRecord> Migrated = NextAwake;
	Migrated[0].WorldTransform.AddToTranslation(FVector(0.0, -1.0, 0.0));
	++Migrated[0].StateRevision;
	const auto Destination = FWorldChunkCoord::FromWorldLocation(Migrated[0].WorldTransform.GetLocation());
	TestTrue(TEXT("已有实体可接收跨 Chunk 权威更新"), Client.Objects->RestorePersistentBatchForTesting(Destination, Migrated, Error));
	Second->SetActorLocation(Desc.WorldTransform.GetLocation() + FVector(0.0, 1.0, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
	++GFrameCounter;
	FWorldDelegates::OnWorldPostActorTick.Broadcast(Client.World, LEVELTICK_All, 1.0f / 60.0f);
	const auto PreviousEntity = Entity;
	TestTrue(TEXT("预测位置在另一侧时仍按权威 Chunk 卸载"), Client.Objects->RuntimeEvictPersistentBatchForTesting(
		Destination, MakeArrayView(&Id, 1), Error));
	TestTrue(TEXT("卸载不销毁仍然相关的复制 Actor"), IsValid(Second));
	TestFalse(TEXT("等待新 Chunk 时暂停刚体碰撞"), Second->GetPhysicsBox()->IsSimulatingPhysics());
	TestTrue(TEXT("从负坐标相邻 Chunk 恢复"), Client.Objects->RestorePersistentBatchForTesting(Destination, Migrated, Error));
	Entity = Client.Objects->FindEntity(Id);
	TestTrue(TEXT("恢复使用新 Generation"), Entity.IsSet() && Entity != PreviousEntity);
	TestTrue(TEXT("同一 Actor 自动重绑新实体"), Client.Objects->GetProxy(Entity) == Second->GetWorldObjectProxyComponent());
	TestTrue(TEXT("临时卸载后恢复预测，不能永久退役"), Second->GetPhysicsBox()->IsSimulatingPhysics());
	TestEqual(TEXT("恢复后仍只有动态碰撞"), Collision->GetStats().CollisionInstanceCount, 0);
	TestTrue(TEXT("接收当前 Actor 的复制销毁"), Second->Destroy(true));
	TestEqual(TEXT("Actor 先销毁时用最后实体姿态接管，不留碰撞空窗"), Collision->GetStats().CollisionInstanceCount, 1);
	return true;
}

#endif
