#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include <limits>

#include "MeteorWorldSubsystem.h"

#include "BuildingWorldSubsystem.h"
#include "Characters/ElementSandboxCharacterMovementComponent.h"
#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Meteor/MeteorProfileSettings.h"
#include "Network/MeteorActivationCausalGate.h"
#include "Network/MeteorStreamingComponent.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Wood/WoodBuildingDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldStorageSubsystem.h"

namespace ElementSandbox::Meteor::Tests
{
	using namespace UE::ElementSandbox::Meteor;

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FMeteorProfileStrikeDelayTest,
		"ElementSandbox.Meteor.Profile.StrikeDelay",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FMeteorProfileStrikeDelayTest::RunTest(const FString& Parameters)
	{
		(void)Parameters;
		TestEqual(TEXT("公平 RHI Profile 可保留 90 秒预热延迟"),
			Profile::SanitizeStrikeDelaySeconds(90.0), 90.0);
		TestEqual(TEXT("过短延迟受下限保护"),
			Profile::SanitizeStrikeDelaySeconds(0.0), Profile::MinStrikeDelaySeconds);
		TestEqual(TEXT("误输入仍受合理上限保护"),
			Profile::SanitizeStrikeDelaySeconds(36000.0), Profile::MaxStrikeDelaySeconds);
		TestEqual(TEXT("非有限输入不会进入 Server Child 命令行"),
			Profile::SanitizeStrikeDelaySeconds(std::numeric_limits<double>::quiet_NaN()),
			Profile::MinStrikeDelaySeconds);
		TestEqual(TEXT("固定 Authority 撞击时刻不受 RHI 就绪时刻影响"),
			Profile::SanitizeAuthorityImpactTimeSeconds(180.0), 180.0);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FMeteorLaneControlSendWindowTest,
		"ElementSandbox.Meteor.Network.LaneControlSendWindow",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FMeteorLaneControlSendWindowTest::RunTest(const FString& Parameters)
	{
		(void)Parameters;
		FMeteorLaneControlSendWindow Window;
		for (uint32 Ordinal = 0; Ordinal < 300; ++Ordinal)
		{
			FMeteorNetPageActivation Activation;
			Activation.Id = {1, static_cast<uint64>(Ordinal / 8 + 1), 1};
			Activation.Ordinals.Add(Ordinal);
			Window.Enqueue(MoveTemp(Activation), false);
		}
		TestEqual(TEXT("控制记录先进入应用层队列"), Window.GetPendingRecordCount(), 300);

		TArray<uint32> DeliveredOrdinals;
		auto ConsumeBatch = [this, &DeliveredOrdinals](const FMeteorNetLaneControlBatch& Batch)
		{
			TestTrue(TEXT("每批控制记录有上限"),
				Batch.Controls.Num() <= FMeteorLaneControlSendWindow::MaximumRecordsPerBatch);
			int32 BatchOrdinalCount = 0;
			for (const FMeteorNetLaneControl& Control : Batch.Controls)
			{
				BatchOrdinalCount += Control.Activation.Ordinals.Num();
				DeliveredOrdinals.Append(Control.Activation.Ordinals);
			}
			TestTrue(TEXT("每批 Ordinal 有上限"),
				BatchOrdinalCount <= FMeteorLaneControlSendWindow::MaximumOrdinalsPerBatch);
		};

		FMeteorNetLaneControlBatch First;
		FMeteorNetLaneControlBatch Second;
		FMeteorNetLaneControlBatch Third;
		TestTrue(TEXT("第一批可发送"), Window.TryBuildBatch(First));
		TestTrue(TEXT("第二批可发送"), Window.TryBuildBatch(Second));
		TestFalse(TEXT("两个未 ACK 批次会形成硬背压"), Window.TryBuildBatch(Third));
		TestEqual(TEXT("在途批次严格受限"), Window.GetInFlightBatchCount(), 2);
		ConsumeBatch(First);
		ConsumeBatch(Second);
		TestTrue(TEXT("ACK 第一批后窗口重新开放"), Window.Acknowledge(First.Sequence));
		TestTrue(TEXT("窗口开放后继续发送"), Window.TryBuildBatch(Third));
		ConsumeBatch(Third);
		TestTrue(TEXT("ACK 第二批"), Window.Acknowledge(Second.Sequence));
		TestTrue(TEXT("ACK 第三批"), Window.Acknowledge(Third.Sequence));

		FMeteorNetLaneControlBatch Remaining;
		while (Window.TryBuildBatch(Remaining))
		{
			ConsumeBatch(Remaining);
			TestTrue(TEXT("剩余批次可 ACK"), Window.Acknowledge(Remaining.Sequence));
		}
		TestEqual(TEXT("300 条记录全部且仅发送一次"), DeliveredOrdinals.Num(), 300);
		for (int32 Index = 0; Index < DeliveredOrdinals.Num(); ++Index)
		{
			TestEqual(TEXT("控制记录保持 Authority 顺序"),
				DeliveredOrdinals[Index], static_cast<uint32>(Index));
		}
		TestEqual(TEXT("发送完成后无残留记录"), Window.GetPendingRecordCount(), 0);
		TestEqual(TEXT("发送完成后无在途批次"), Window.GetInFlightBatchCount(), 0);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeteorSettlementBackpressureTest,
		"ElementSandbox.Meteor.Network.SettlementSharesControlBackpressure",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FMeteorSettlementBackpressureTest::RunTest(const FString& Parameters)
	{
		FMeteorLaneControlSendWindow Window;
		TArray<FMeteorNetSettlement> Settlements;
		for (uint32 Index = 0; Index < 65536; ++Index)
		{
			Settlements.Add({1, Index, FWorldEntityId(Index + 1)});
		}
		Window.EnqueueSettlements(Settlements);
		FMeteorNetPageActivation Activation;
		Activation.Id = {1, 1, 1};
		Activation.Ordinals.Add(0);
		Window.Enqueue(Activation, false);
		TestTrue(TEXT("大积压只估算下一批，不按全部结算量阻止发送"),
			Window.GetNextBatchEstimatedBytes() > 0 && Window.GetNextBatchEstimatedBytes() < 32 * 1024);
		FMeteorNetLaneControlBatch First, Second, Blocked;
		TestTrue(TEXT("积压结算仍允许第一批源控制同行"), Window.TryBuildBatch(First) && First.Controls.Num() == 1);
		TestTrue(TEXT("第二批可以仅有结算记录"), Window.TryBuildBatch(Second));
		TestFalse(TEXT("未 ACK 时结算不能继续灌入可靠通道"), Window.TryBuildBatch(Blocked));
		TestEqual(TEXT("窗口满时没有发送需求"), Window.GetNextBatchEstimatedBytes(), 0);
		uint32 Received = 0;
		bool bOrderedAndBounded = true;
		const auto Consume = [&Received, &bOrderedAndBounded](const FMeteorNetLaneControlBatch& Batch)
		{
			bOrderedAndBounded &= !Batch.Settlements.IsEmpty()
				&& Batch.Settlements.Num() <= FMeteorLaneControlSendWindow::MaximumSettlementsPerBatch;
			for (const auto& Settlement : Batch.Settlements)
			{
				bOrderedAndBounded &= Settlement.DebrisOrdinal == Received
					&& Settlement.WorldEntityId == FWorldEntityId(Received + 1);
				++Received;
			}
		};
		Consume(First);
		Consume(Second);
		Window.Acknowledge(First.Sequence);
		Window.Acknowledge(Second.Sequence);
		while (Window.TryBuildBatch(First))
		{
			Consume(First);
			Window.Acknowledge(First.Sequence);
		}
		TestTrue(TEXT("跨队列收缩和 ACK 的结算有界且不乱序"), bOrderedAndBounded);
		TestEqual(TEXT("所有结算身份仅发送一次"), Received, 65536u);
		TestEqual(TEXT("结算积压耗尽"), Window.GetPendingSettlementCount(), 0);
		TestEqual(TEXT("无在途批次"), Window.GetInFlightBatchCount(), 0);
		return true;
	}

	struct FMeteorAuthorityWorld final
	{
		explicit FMeteorAuthorityWorld(const bool bSimulatePhysics = false)
		{
			UWorld::InitializationValues InitializationValues;
			InitializationValues
				.CreatePhysicsScene(true)
				.ShouldSimulatePhysics(bSimulatePhysics)
				.EnableTraceCollision(true)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE,
				false,
				TEXT("MeteorAuthorityIntegration"),
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
			Storage = World->GetSubsystem<UWorldStorageSubsystem>();
			Buildings = World->GetSubsystem<UBuildingWorldSubsystem>();
			WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			Meteor = World->GetSubsystem<UMeteorWorldSubsystem>();
		}

		~FMeteorAuthorityWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		void Advance(const float DeltaSeconds) const
		{
			++GFrameCounter;
			World->Tick(LEVELTICK_All, DeltaSeconds);
			// Simple Automation 在同一个 Engine frame 内运行，因此显式推进两个
			// UTickableWorldSubsystem。Authority 的真实运行路径仍由 Engine 逐帧 Tick。
			Storage->Tick(DeltaSeconds);
			Meteor->Tick(DeltaSeconds);
			FWorldDelegates::OnWorldPostActorTick.Broadcast(World, LEVELTICK_All, DeltaSeconds);
			FPlatformProcess::SleepNoStats(0.001f);
		}

		UWorld* World = nullptr;
		UWorldStorageSubsystem* Storage = nullptr;
		UBuildingWorldSubsystem* Buildings = nullptr;
		UWorldObjectWorldSubsystem* WorldObjects = nullptr;
		UMeteorWorldSubsystem* Meteor = nullptr;
	};

	struct FMeteorClientProjectionWorld final
	{
		FMeteorClientProjectionWorld()
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
				TEXT("MeteorClientCausalProjection"),
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&InitializationValues,
				true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NM_Client);
			World->InitWorld(InitializationValues);
			World->UpdateWorldComponents(true, false);
			Storage = World->GetSubsystem<UWorldStorageSubsystem>();
			WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
		}

		~FMeteorClientProjectionWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		UWorld* World = nullptr;
		UWorldStorageSubsystem* Storage = nullptr;
		UWorldObjectWorldSubsystem* WorldObjects = nullptr;
	};

	FWorldObjectEntityHandle CreateDormant(
		UWorldObjectWorldSubsystem& WorldObjects,
		UWorldObjectDefinition& Definition,
		const FVector& Location)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = &Definition;
		Desc.WorldTransform = FTransform(Location);
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		return WorldObjects.CreateEntity(Desc);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorSourceCommitCausalGateTest,
	"ElementSandbox.Meteor.Network.SourceDestroyPrecedesActivationAndDuplicateTombstoneIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorSourceCommitCausalGateTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Meteor::Tests;
	FMeteorAuthorityWorld Authority;
	FMeteorClientProjectionWorld Client;
	if (!Authority.Storage || !Authority.WorldObjects || !Client.Storage || !Client.WorldObjects)
	{
		AddError(TEXT("因果门测试所需 WorldStorage/WorldObject Subsystem 未初始化"));
		return false;
	}

	UWorldObjectDefinition* TreeDefinition = Authority.WorldObjects->FindDefinition(
		GetDefault<USettlementTreeDefinition>()->DefinitionId);
	if (!TreeDefinition)
	{
		AddError(TEXT("测试树 Definition 未注册"));
		return false;
	}
	const FWorldObjectEntityHandle AuthoritySource = CreateDormant(
		*Authority.WorldObjects, *TreeDefinition, FVector(100.0, 200.0, 0.0));
	const FWorldObjectEntityHandle AuthoritySourceSecond = CreateDormant(
		*Authority.WorldObjects, *TreeDefinition, FVector(300.0, 400.0, 0.0));
	const FWorldObjectWorldIdentityFragment* AuthorityIdentity = AuthoritySource.IsSet()
		? Authority.WorldObjects->GetRegistry()
			.FindFragment<FWorldObjectWorldIdentityFragment>(AuthoritySource)
		: nullptr;
	const FWorldObjectWorldIdentityFragment* AuthorityIdentitySecond = AuthoritySourceSecond.IsSet()
		? Authority.WorldObjects->GetRegistry()
			.FindFragment<FWorldObjectWorldIdentityFragment>(AuthoritySourceSecond)
		: nullptr;
	FWorldPersistentEntityRecord Record;
	FWorldPersistentEntityRecord SecondRecord;
	FString CaptureError;
	TestTrue(TEXT("Authority 捕获第一条源投影记录"), AuthorityIdentity
		&& Authority.Storage->CaptureResidentRecord(
			AuthorityIdentity->WorldEntityId,
			Record,
			CaptureError));
	TestTrue(TEXT("Authority 捕获第二条源投影记录"), AuthorityIdentitySecond
		&& Authority.Storage->CaptureResidentRecord(
			AuthorityIdentitySecond->WorldEntityId,
				SecondRecord,
				CaptureError));
	if (!Record.IsValid() || !SecondRecord.IsValid())
	{
		AddError(FString::Printf(TEXT("源记录无效：%s"), *CaptureError));
		return false;
	}

	TestTrue(TEXT("客户端先恢复第一条源投影"), Client.Storage->ApplyNetworkUpsert(Record));
	TestTrue(TEXT("客户端先恢复第二条源投影"), Client.Storage->ApplyNetworkUpsert(SecondRecord));
	const FWorldObjectEntityHandle ClientSource = Client.WorldObjects->FindEntity(Record.EntityId);
	const FWorldObjectEntityHandle ClientSourceSecond =
		Client.WorldObjects->FindEntity(SecondRecord.EntityId);
	TestTrue(TEXT("Activate 前两条树木投影均存在"),
		Client.WorldObjects->IsEntityAlive(ClientSource)
			&& Client.WorldObjects->IsEntityAlive(ClientSourceSecond));

	const TArray<FWorldNetworkEntityRemoval> Removals{
		{Record.EntityId, Record.StateRevision + 1, true},
		{SecondRecord.EntityId, SecondRecord.StateRevision + 1, true}};
	TestTrue(TEXT("SourceCommit 因果门把多条 GameplayDestroy 合并提交"),
		ApplyMeteorSourceTombstones(*Client.Storage, Removals));
	TestFalse(TEXT("Lane 获准发布前第一条源 ECS 投影已经退出"),
		Client.WorldObjects->IsEntityAlive(Client.WorldObjects->FindEntity(Record.EntityId)));
	TestFalse(TEXT("Lane 获准发布前第二条源 ECS 投影已经退出"),
		Client.WorldObjects->IsEntityAlive(Client.WorldObjects->FindEntity(SecondRecord.EntityId)));

	TestTrue(TEXT("相同批量 Tombstone 重放仍幂等成功"),
		ApplyMeteorSourceTombstones(*Client.Storage, Removals));
	TestFalse(TEXT("重复批次只做幂等确认且两条源都不复活"),
		Client.WorldObjects->IsEntityAlive(Client.WorldObjects->FindEntity(Record.EntityId))
			|| Client.WorldObjects->IsEntityAlive(Client.WorldObjects->FindEntity(SecondRecord.EntityId)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorAuthorityWaveAndSettlementTest,
	"ElementSandbox.Meteor.Authority.WaveDomainsAndSettlement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorAuthorityWaveAndSettlementTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Meteor::Tests;
	FMeteorAuthorityWorld Harness(true);
	if (!Harness.Storage || !Harness.Buildings || !Harness.WorldObjects || !Harness.Meteor)
	{
		AddError(TEXT("Meteor 集成测试所需 WorldSubsystem 未初始化"));
		return false;
	}

	USettlementTreeDefinition* TreeDefinition =
		Cast<USettlementTreeDefinition>(Harness.WorldObjects->FindDefinition(
			GetDefault<USettlementTreeDefinition>()->DefinitionId));
	UWorldObjectDefinition* WoodBlockDefinition = Harness.WorldObjects->FindDefinition(
		GetDefault<UWoodBlockWorldObjectDefinition>()->DefinitionId);
	UWoodBuildingDefinition* BuildingDefinition = NewObject<UWoodBuildingDefinition>(Harness.World);
	TestTrue(TEXT("生产 Building Definition 可用于冲击波"),
		BuildingDefinition && BuildingDefinition->Initialize(
			TEXT("Test.Meteor.WoodBuilding"), FVector(200.0, 200.0, 400.0)));
	if (!TreeDefinition || !WoodBlockDefinition || !BuildingDefinition
		|| !BuildingDefinition->HasValidDefinitionId())
	{
		AddError(TEXT("生产树木、木块或 Building Definition 未正确注册"));
		return false;
	}

	const FWorldObjectEntityHandle Tree = CreateDormant(
		*Harness.WorldObjects, *TreeDefinition, FVector::ZeroVector);
	const FBuildEntityHandle Building = Harness.Buildings->CreateEntity(
		*BuildingDefinition, FTransform(FVector(1500.0, 0.0, 0.0)));
	const FWorldObjectEntityHandle ExistingWoodBlock = CreateDormant(
		*Harness.WorldObjects, *WoodBlockDefinition, FVector(1000.0, 250.0, 20.0));
	const FWorldObjectEntityHandle RecycledSource = CreateDormant(
		*Harness.WorldObjects, *TreeDefinition, FVector(2100.0, 0.0, 0.0));
	TestTrue(TEXT("创建可破坏树木"), Tree.IsSet());
	TestTrue(TEXT("创建可破坏 Building"), Building.IsSet());
	TestTrue(TEXT("创建不可二次破坏木块"), ExistingWoodBlock.IsSet());
	TestTrue(TEXT("创建用于 Generation 复用的源"), RecycledSource.IsSet());
	if (!Tree.IsSet() || !Building.IsSet() || !ExistingWoodBlock.IsSet() || !RecycledSource.IsSet())
	{
		return false;
	}
	const FWorldResidencySourceHandle ResidencySource =
		Harness.Storage->RegisterResidencySource(FVector::ZeroVector);
	TestTrue(TEXT("玩家附近 Residency 在 Burst 结算期间保持源与产物投影"), ResidencySource.IsSet());

	FMeteorRuntimeConfig Config = Harness.Meteor->GetRuntimeConfig();
	Config.ShockwaveRadius = 2500.0f;
	Config.ImpactCoreRadius = 500.0f;
	Config.ShockwaveSpeed = 2500.0f;
	Config.NetworkLeadSeconds = 0.5f;
	Config.EncodingEstimateSeconds = 0.05f;
	Config.QueueSafetySeconds = 0.1f;
	Config.MaximumQueryTilesPerPump = 16;
	Config.MaximumDestructionTargetsPerPump = 64;
	Config.LocalServerGameplayBudgetMilliseconds = 25.0f;
	Config.LocalServerWorkerConcurrency = 2;
	Config.MaximumSettlementBatchSize = 128;
	// 本测试验证破坏事务与落地 WorldObject 的发布，产物必须留在测试 Residency 范围内。
	// 集成测试缩短弹道，避免把 Residency 驱逐语义误判为 Settlement 失败。
	Config.RadialStrength = 0.0f;
	Config.UpwardStrength = 0.0f;
	Config.DebrisSpeedRange = FVector2f(500.0f, 1500.0f);
	Config.DebrisLowElevationDegrees = FVector2f(5.0f, 10.0f);
	Config.DebrisMediumElevationDegrees = FVector2f(10.0f, 20.0f);
	Config.DebrisHighElevationDegrees = FVector2f(20.0f, 30.0f);
	Config.GravityZ = -980.0f;
	Harness.Meteor->OverrideRuntimeConfigForTesting(Config);
	FVector MapImpactLocation;
	TestTrue(TEXT("陨石落点可从角色前方的当前 Resident Chunk 稳定解析"),
		Harness.Meteor->TryGetMapImpactLocation(
			FVector(5000.0 - Config.MeteorShowcasePreferredDistance, 5000.0, 0.0),
			FVector::ForwardVector, MapImpactLocation));
	TestTrue(TEXT("极小地图也只选择角色前方近距离的已有宿主 Chunk"),
		MapImpactLocation.Equals(FVector(5000.0, 5000.0, Config.GroundPlaneZ), 1.0));

	TArray<FMeteorTrajectoryPage> PublishedPages;
	TArray<FMeteorTrajectoryActivation> PublishedActivations;
	TArray<FMeteorTrajectoryActivation> PublishedCancellations;
	TArray<FMeteorSettlementMapping> PublishedSettlements;
	int32 FirstPreparedFrame = INDEX_NONE;
		int32 FirstActivationFrame = INDEX_NONE;
		int32 CurrentFrame = INDEX_NONE;
		bool bReservedWithoutEntities = true;
		const FDelegateHandle PageHandle = Harness.Meteor->OnTrajectoryPagePrepared().AddLambda(
			[&PublishedPages, &FirstPreparedFrame, &CurrentFrame, &bReservedWithoutEntities, &Harness](const FMeteorTrajectoryPage& Page)
			{
				PublishedPages.Add(Page);
				for (FWorldEntityId Id : Page.WorldEntityIds)
					bReservedWithoutEntities &= Id.IsSet() && !Harness.WorldObjects->FindEntity(Id).IsSet();
				if (FirstPreparedFrame == INDEX_NONE) FirstPreparedFrame = CurrentFrame;
		});
	const FDelegateHandle ActivationHandle = Harness.Meteor->OnTrajectoryActivated().AddLambda(
		[&PublishedActivations, &FirstActivationFrame, &CurrentFrame](const FMeteorTrajectoryActivation& Activation)
		{
			PublishedActivations.Add(Activation);
			if (FirstActivationFrame == INDEX_NONE) FirstActivationFrame = CurrentFrame;
		});
	const FDelegateHandle CancellationHandle = Harness.Meteor->OnTrajectoryCanceled().AddLambda(
		[&PublishedCancellations](const FMeteorTrajectoryActivation& Cancellation)
		{
			PublishedCancellations.Add(Cancellation);
		});
	const FDelegateHandle SettlementHandle = Harness.Meteor->OnSettlementPublished().AddLambda(
		[&PublishedSettlements](const TConstArrayView<FMeteorSettlementMapping> Mappings)
		{
			PublishedSettlements.Append(Mappings);
		});

	const FWorldStorageRuntimeStats StorageBeforeStrike = Harness.Storage->GetRuntimeStats();
	FMeteorBurstId BurstId;
	// 测试 World 以远高于实时的速度推进；给异步基线封口保留与正式 6 秒下落
	// 等价的准备窗，避免把测试机磁盘/线程调度速度误判成撞击时序错误。
	const double ScheduledImpactTime = Harness.World->GetTimeSeconds() + 5.0;
	TestTrue(TEXT("Authority 排程唯一 Burst"), Harness.Meteor->ScheduleStrike(
		FVector::ZeroVector, ScheduledImpactTime, BurstId));
	TestTrue(TEXT("Burst 身份有效"), BurstId.IsSet());
	const FWorldStorageRuntimeStats StorageAfterStrike = Harness.Storage->GetRuntimeStats();
	TestEqual(TEXT("Meteor 排程不新增 Chunk 加载"),
		StorageAfterStrike.PendingLoadCount, StorageBeforeStrike.PendingLoadCount);
	TestEqual(TEXT("Meteor 排程不改变当前 Resident Chunk 集合"),
		StorageAfterStrike.ResidentChunkCount, StorageBeforeStrike.ResidentChunkCount);
	int32 SimulationFrame = 0;
	int32 TreeDestroyedFrame = INDEX_NONE;
	int32 BuildingDestroyedFrame = INDEX_NONE;
	const auto HasPreparedRecycledSource = [&PublishedPages]()
	{
		for (const FMeteorTrajectoryPage& Page : PublishedPages)
		{
			for (const FVector3f& LocalStart : Page.LocalStarts)
			{
				const FVector3d WorldStart = Page.PageOrigin + FVector3d(LocalStart);
				if (FMath::Abs(WorldStart.X - 2100.0) < 250.0) return true;
			}
		}
		return false;
	};
	for (; SimulationFrame < 180 && !HasPreparedRecycledSource(); ++SimulationFrame)
	{
		CurrentFrame = SimulationFrame;
		Harness.Advance(1.0f / 30.0f);
		if (TreeDestroyedFrame == INDEX_NONE && !Harness.WorldObjects->IsEntityAlive(Tree))
		{
			TreeDestroyedFrame = CurrentFrame;
		}
		if (BuildingDestroyedFrame == INDEX_NONE && !Harness.Buildings->IsEntityAlive(Building))
		{
			BuildingDestroyedFrame = CurrentFrame;
		}
	}
	TestTrue(TEXT("旧 Generation 的 Payload 已在源提交前完成预流送"), HasPreparedRecycledSource());
	TestTrue(TEXT("波前前旧 Generation 可被移除"), Harness.WorldObjects->DestroyEntity(RecycledSource));
	const FWorldObjectEntityHandle ReplacementSource = CreateDormant(
		*Harness.WorldObjects, *TreeDefinition, FVector(2100.0, 0.0, 0.0));
	TestTrue(TEXT("同一 Handle Slot 的新 Generation 被重新注入"), ReplacementSource.IsSet()
		&& ReplacementSource.GetSlot() == RecycledSource.GetSlot()
		&& ReplacementSource != RecycledSource);

	int32 ReplacementDestroyedFrame = INDEX_NONE;
	double PreviousWaveRadius = 0.0;
	// 公里级展示轨迹会在空中保持约一分钟；集成测试推进完整权威时间，不能把旧版
	// 40 秒短抛物线等待上限误当成 Runtime 失败。
	for (int32 Frame = 0; Frame < 3600 && Harness.Meteor->HasActiveBurst(); ++Frame)
	{
		CurrentFrame = SimulationFrame++;
		Harness.Advance(1.0f / 30.0f);
		const FMeteorAuthorityStats Stats = Harness.Meteor->GetAuthorityStats();
		TestTrue(TEXT("已发布冲击波半径单调不回退"),
			Stats.PublishedWaveRadius + UE_KINDA_SMALL_NUMBER >= PreviousWaveRadius);
		PreviousWaveRadius = Stats.PublishedWaveRadius;
		if (TreeDestroyedFrame == INDEX_NONE && !Harness.WorldObjects->IsEntityAlive(Tree))
		{
			TreeDestroyedFrame = CurrentFrame;
		}
		if (BuildingDestroyedFrame == INDEX_NONE && !Harness.Buildings->IsEntityAlive(Building))
		{
			BuildingDestroyedFrame = CurrentFrame;
		}
		if (ReplacementDestroyedFrame == INDEX_NONE && !Harness.WorldObjects->IsEntityAlive(ReplacementSource))
		{
			ReplacementDestroyedFrame = CurrentFrame;
		}
	}

	Harness.Meteor->OnTrajectoryPagePrepared().Remove(PageHandle);
	Harness.Meteor->OnTrajectoryActivated().Remove(ActivationHandle);
	Harness.Meteor->OnTrajectoryCanceled().Remove(CancellationHandle);
	Harness.Meteor->OnSettlementPublished().Remove(SettlementHandle);
	TestTrue(TEXT("近处树木被冲击波破坏"), TreeDestroyedFrame != INDEX_NONE);
	TestTrue(TEXT("远处 Building 被冲击波破坏"), BuildingDestroyedFrame != INDEX_NONE);
	TestTrue(TEXT("波前前复用 Slot 的新 Generation 仍被捕获"), ReplacementDestroyedFrame != INDEX_NONE);
	TestTrue(*FString::Printf(TEXT("冲击波按距离由近到远提交（Tree=%d, Building=%d）"),
		TreeDestroyedFrame, BuildingDestroyedFrame),
		TreeDestroyedFrame != INDEX_NONE && BuildingDestroyedFrame > TreeDestroyedFrame);
	TestTrue(TEXT("木块默认不可破坏且不会递归参与本轮"),
		Harness.WorldObjects->IsEntityAlive(ExistingWoodBlock));
	TestFalse(TEXT("全部轨迹与落地结算完成后 Burst 退出"), Harness.Meteor->HasActiveBurst());
	TestTrue(TEXT("Building 与树木产物编译为解析轨迹页"), !PublishedPages.IsEmpty());
	TestTrue(TEXT("Payload 在首个源 GameplayDestroy 前完成预发布"),
		FirstPreparedFrame != INDEX_NONE && FirstPreparedFrame < TreeDestroyedFrame);
	TestEqual(TEXT("核心区首批 Activate 与近处源销毁发生在同一 Authority Pump"),
		FirstActivationFrame, TreeDestroyedFrame);
	TestTrue(TEXT("源提交后仅发布轻量 Lane Activation"), !PublishedActivations.IsEmpty());
	TestTrue(TEXT("三个被毁宿主均按斧头规则生成 3–6 个产品 Lane"),
		PublishedSettlements.Num() >= 9 && PublishedSettlements.Num() <= 18);

		TestTrue(TEXT("Prepare 预留最终身份但未创建 ECS"), bReservedWithoutEntities);
		TMap<uint32, FWorldEntityId> ReservedIds;
		TSet<FWorldEntityId> UniqueReservedIds;
		int32 PublishedLaneCount = 0;
	int32 ActivatedLaneCount = 0;
	bool bAllFlightScalesUniform = true;
		for (const FMeteorTrajectoryPage& Page : PublishedPages)
		{
			PublishedLaneCount += Page.Num();
			for (int32 Lane = 0; Lane < Page.Num(); ++Lane)
			{
				ReservedIds.Add(Page.Ordinals[Lane], Page.WorldEntityIds[Lane]);
				UniqueReservedIds.Add(Page.WorldEntityIds[Lane]);
			}
		TestTrue(TEXT("网络提前量只控制调度，不推迟权威起飞时刻"),
			Page.ValidFromSeconds <= ScheduledImpactTime
				+ (Config.ShockwaveRadius - Config.ImpactCoreRadius) / Config.ShockwaveSpeed
				+ UE_KINDA_SMALL_NUMBER);
		TestEqual(TEXT("解析页使用斧头同源的固定木块产品 Definition"),
			Page.RenderArchetypeId, WoodBlockDefinition->DefinitionId);
		for (const FVector3f Scale : Page.Scales)
		{
			TestTrue(TEXT("飞行木块缩放有限且为正"),
				Scale.X > 0.0f && Scale.Y > 0.0f && Scale.Z > 0.0f
				&& !Scale.ContainsNaN());
			bAllFlightScalesUniform &= FMath::IsNearlyEqual(Scale.X, Scale.Y)
				&& FMath::IsNearlyEqual(Scale.Y, Scale.Z);
		}
		TestTrue(TEXT("轨迹页使用统一地面弹道内核"),
			Page.Kernel == EMeteorTrajectoryKernel::BallisticGroundPlane);
	}
		TestTrue(TEXT("房屋与树木飞行产物全部保持普通木块均匀尺度"),
			bAllFlightScalesUniform);
		TestEqual(TEXT("跨来源及轨迹页预留身份不重复"), UniqueReservedIds.Num(), PublishedLaneCount);
	for (const FMeteorTrajectoryActivation& Activation : PublishedActivations)
	{
		TestTrue(TEXT("Activate 携带真正源提交的权威起飞时刻"),
			FMath::IsFinite(Activation.AuthorityStartTimeSeconds)
			&& Activation.AuthorityStartTimeSeconds >= ScheduledImpactTime);
		TestTrue(TEXT("Activate 携带源 WorldEntityId 与 Tombstone Revision"),
			Activation.SourceWorldEntityId.IsSet()
			&& Activation.SourceTombstoneRevision > 0);
		ActivatedLaneCount += Activation.Ordinals.Num();
	}
	TestTrue(TEXT("旧 Generation 已预流送 Lane 不会被误激活"),
		PublishedLaneCount > ActivatedLaneCount);
	TestTrue(TEXT("旧 Generation 的预流送 Lane 发送轻量 Cancel"),
		!PublishedCancellations.IsEmpty());
	TestEqual(TEXT("每条飞行 Lane 最终恰好得到一个 Settlement"),
		ActivatedLaneCount, PublishedSettlements.Num());
	const FMeteorAuthorityStats FinalStats = Harness.Meteor->GetAuthorityStats();
	TestTrue(TEXT("统计记录核心候选"), FinalStats.CoreCandidateCount > 0);
	TestTrue(TEXT("撞击帧至少提交一个核心源"), FinalStats.ImpactFrameDestroyedTargets > 0);
	TestTrue(TEXT("首批 Activate 至少包含一批斧头同源产品"),
		FinalStats.FirstActivationLaneCount >= 3);
	TestEqual(TEXT("累计 Activate 统计不再冒充当前空中数量"),
		FinalStats.TotalActivatedLaneCount, static_cast<uint32>(ActivatedLaneCount));
	TestTrue(TEXT("撞击到首批 Activate 延迟被显式统计"),
		FinalStats.ImpactToFirstActivationMilliseconds >= 0.0);

		for (const FMeteorSettlementMapping& Mapping : PublishedSettlements)
		{
			TestTrue(TEXT("Settlement 沿用 Prepare 预留的最终身份"),
				ReservedIds.FindRef(Mapping.Debris.DebrisOrdinal) == Mapping.WorldEntityId);
		const FWorldObjectEntityHandle Product = Harness.WorldObjects->FindEntity(Mapping.WorldEntityId);
		const FWorldObjectMotionFragment* Motion =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Product);
		const FWorldObjectDefinitionFragment* Definition =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectDefinitionFragment>(Product);
		const FWorldObjectTransformFragment* Transform =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Product);
			TestTrue(TEXT("落地后才创建普通实体，并直接进入 Dormant"),
			Product.IsSet() && Motion && Motion->State == EWorldObjectMotionState::Dormant);
		TestTrue(TEXT("每个落地产物仍是同一规范可拾取木块 Definition"),
			Definition && Definition->Definition.Get() == WoodBlockDefinition);
		const FVector SettlementScale = Transform
			? Transform->WorldTransform.GetScale3D().GetAbs() : FVector::ZeroVector;
		TestTrue(TEXT("落地拾取木块继续保持飞行期的均匀产品尺度"),
			Transform
			&& FMath::IsNearlyEqual(SettlementScale.X, SettlementScale.Y)
			&& FMath::IsNearlyEqual(SettlementScale.Y, SettlementScale.Z));
		TestFalse(TEXT("解析飞行产物落地后不遗留 Chaos Physics Proxy"),
			Harness.WorldObjects->GetProxy(Product) != nullptr);
		const FWorldObjectPhysicsBodyFragment* PhysicsBody =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(Product);
		TestTrue(TEXT("陨石落地木块沿用斧头木块的 LooseDebris 接触策略"),
			PhysicsBody && PhysicsBody->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris);
	}
	if (!PublishedSettlements.IsEmpty())
	{
		const FWorldEntityId ProductId = PublishedSettlements[0].WorldEntityId;
		const FWorldObjectEntityHandle Product = Harness.WorldObjects->FindEntity(ProductId);
		const FWorldObjectTransformFragment* Transform =
			Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Product);
		UWorldObjectCollisionWorldSubsystem* Collision =
			Harness.World->GetSubsystem<UWorldObjectCollisionWorldSubsystem>();
		if (TestTrue(TEXT("落地木块可进入普通近场碰撞路径"), Transform && Collision))
		{
			const FVector Center = Transform->WorldTransform.GetLocation();
			FWorldObjectCollisionSource Source;
			Source.SubjectLocation = Center - FVector(100.0, 0.0, 0.0);
			Source.ViewLocation = Source.SubjectLocation;
			Source.PawnContactBounds = WoodBlockDefinition->InteractionLocalBounds
				.TransformBy(Transform->WorldTransform).ExpandBy(10.0);
			Source.ImmediateBounds = FBox(Center - FVector(400.0), Center + FVector(400.0));
			Source.PrefetchBounds = Source.ImmediateBounds;
			Source.RetentionBounds = Source.ImmediateBounds.ExpandBy(300.0);
			Source.Revision = 1;
			const FWorldObjectCollisionSourceHandle ContactSource = Collision->RegisterSource(Source);
			TestTrue(TEXT("角色近场 Source 可注册"), ContactSource.IsSet());
			Collision->FlushImmediateCollisionChanges();
			TestNull(TEXT("角色静止时陨石木块保持 Dormant，无常驻物理 Actor"),
				Harness.WorldObjects->GetProxy(Product));

			Source.Velocity = FVector(500.0, 0.0, 0.0);
			Source.Revision = 2;
			TestTrue(TEXT("角色移动更新接触走廊"), Collision->UpdateSource(ContactSource, Source));
			Collision->FlushImmediateCollisionChanges();
			UWorldObjectProxyComponent* Proxy = Harness.WorldObjects->GetProxy(Product);
			AWorldObjectPhysicsProxyActor* PhysicsActor = Proxy
				? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
			UBoxComponent* PhysicsBox = PhysicsActor ? PhysicsActor->GetPhysicsBox() : nullptr;
			const FWorldObjectMotionFragment* Motion =
				Harness.WorldObjects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Product);
			TestTrue(TEXT("接触走廊以原身份将陨石木块从 Dormant 唤醒为 Physics"),
				Harness.WorldObjects->FindEntity(ProductId) == Product
				&& Motion && Motion->State == EWorldObjectMotionState::Physics);
			if (TestTrue(TEXT("陨石木块预唤醒后立即释放 Authority Chaos"),
				PhysicsActor && PhysicsActor->IsPhysicsReleased()
				&& PhysicsBox && PhysicsBox->IsSimulatingPhysics()))
			{
				TestTrue(TEXT("预唤醒不提前踢走木块"), PhysicsBox->GetPhysicsLinearVelocity().IsNearlyZero());
				UElementSandboxCharacterMovementComponent* Movement =
					NewObject<UElementSandboxCharacterMovementComponent>();
				FHitResult Contact(PhysicsActor, PhysicsBox, Center, -FVector::ForwardVector);
				Contact.bBlockingHit = true;
				// 排除重力；服务器同一物理步前可能处理多个 Client Move，推速不能重复叠加。
				PhysicsBox->SetEnableGravity(false);
				for (int32 MoveIndex = 0; MoveIndex < 3; ++MoveIndex)
					Movement->ApplyImpactPhysicsForces(Contact, FVector::ZeroVector, Source.Velocity);
				Harness.Advance(1.0f / 60.0f);
				const FVector KickVelocity = PhysicsBox->GetPhysicsLinearVelocity();
				TestTrue(*FString::Printf(TEXT("真实角色接触能水平踢动陨石木块，速度受限且不产生向上冲量（速度=%s）"),
					*KickVelocity.ToString()),
					KickVelocity.X > 0.0 && KickVelocity.Size2D() <= 600.0
					&& FMath::IsNearlyZero(KickVelocity.Z));
				PhysicsActor->GatherCurrentMovement();
				TestTrue(TEXT("踢动后的陨石木块复制完整物理状态供客户端预测"),
					PhysicsActor->GetReplicatedMovement().bRepPhysics);
				TestEqual(TEXT("陨石木块接入原生 Predictive Interpolation"),
					PhysicsActor->GetPhysicsReplicationMode(), EPhysicsReplicationMode::PredictiveInterpolation);

				TArray<FWorldPersistentEntityRecord> Records;
				FString CaptureError;
				if (TestTrue(TEXT("踢醒的陨石木块可捕获普通 Physics 记录"),
					Harness.WorldObjects->CapturePersistentBatchForTesting(
						MakeArrayView(&ProductId, 1), Records, CaptureError))
					&& TestEqual(TEXT("只捕获目标木块"), Records.Num(), 1))
				{
					FMeteorClientProjectionWorld Client;
					TestTrue(TEXT("客户端可接收踢醒后的陨石木块记录"),
						Client.Storage->ApplyNetworkUpsert(Records[0]));
					const FWorldObjectEntityHandle ClientProduct = Client.WorldObjects->FindEntity(ProductId);
					const FWorldObjectPhysicsBodyFragment* ClientPhysics =
						Client.WorldObjects->GetRegistry().FindFragment<FWorldObjectPhysicsBodyFragment>(ClientProduct);
					TestTrue(TEXT("客户端记录保留同一 LooseDebris 策略并等待 Authority Actor 绑定"),
						ClientPhysics && ClientPhysics->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris
						&& Client.WorldObjects->GetProxy(ClientProduct) == nullptr);

					FMeteorAuthorityWorld Restored;
					TestTrue(TEXT("Physics 状态的陨石木块可通过正式存档路径恢复"),
						Restored.WorldObjects->RestorePersistentBatchForTesting(
							FWorldChunkCoord::FromWorldLocation(Records[0].WorldTransform.GetLocation()),
							Records, CaptureError));
					UWorldObjectProxyComponent* RestoredProxy =
						Restored.WorldObjects->GetProxy(Restored.WorldObjects->FindEntity(ProductId));
					const AWorldObjectPhysicsProxyActor* RestoredActor = RestoredProxy
						? Cast<AWorldObjectPhysicsProxyActor>(RestoredProxy->GetOwner()) : nullptr;
					TestTrue(TEXT("重新加载仍能重建 LooseDebris Physics Proxy"),
						RestoredActor && RestoredActor->GetConfiguredCollisionPolicy()
							== EWorldObjectPhysicsCollisionPolicy::LooseDebris);
				}
			}
			TestTrue(TEXT("接触验证完成后释放近场 Source"), Collision->UnregisterSource(ContactSource));
		}
	}
	TestTrue(TEXT("测试 Residency Source 正常释放"),
		Harness.Storage->UnregisterResidencySource(ResidencySource));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeteorDueWorkPriorityTest,
	"ElementSandbox.Meteor.Authority.DueDestructionPrecedesPreparationBacklog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorDueWorkPriorityTest::RunTest(const FString&)
{
	using namespace ElementSandbox::Meteor::Tests;
	FMeteorAuthorityWorld Harness;
	UWorldObjectDefinition* Definition = Harness.WorldObjects->FindDefinition(
		GetDefault<USettlementTreeDefinition>()->DefinitionId);
	if (!TestNotNull(TEXT("生产树木定义已注册"), Definition)) return false;
	TArray<FWorldObjectCreateDesc> Descs;
	for (int32 I = 0; I < 4096; ++I)
	{
		auto& Desc = Descs.AddDefaulted_GetRef();
		Desc.Definition = Definition;
		Desc.WorldTransform = FTransform(FVector((I % 64) * 100.0, (I / 64) * 100.0, 0));
		Desc.MotionState = EWorldObjectMotionState::Dormant;
	}
	FWorldObjectStagedCreateBatch Batch;
	TArray<FWorldObjectEntityHandle> Entities;
	if (!TestTrue(TEXT("创建足以持续占满预演预算的候选"),
		Harness.WorldObjects->StageCreateEntities(Descs, Batch)
		&& Harness.WorldObjects->CommitStagedCreateEntities(Batch, Entities))) return false;
	const auto Residency = Harness.Storage->RegisterResidencySource(FVector::ZeroVector);
	// 隔离调度顺序，先封口造景事务，避免把合法的异步存档屏障等待当成销毁饥饿。
	TestTrue(TEXT("造景事务开始封口"), Harness.Storage->RequestCheckpoint());
	for (int32 I = 0; I < 1000; ++I)
	{
		Harness.Advance(1.0f / 60.0f);
		const auto StorageStats = Harness.Storage->GetRuntimeStats();
		if (StorageStats.DirtyEntityCount == 0 && !StorageStats.bCheckpointInFlight) break;
	}
	TestEqual(TEXT("撞击前造景事务已封口"), Harness.Storage->GetRuntimeStats().DirtyEntityCount, 0);
	FMeteorRuntimeConfig Config = Harness.Meteor->GetRuntimeConfig();
	Config.ShockwaveRadius = 20000;
	Config.ImpactCoreRadius = 10000;
	Config.LocalServerGameplayBudgetMilliseconds = 0.25f;
	Config.LocalServerWorkerConcurrency = 1;
	Harness.Meteor->OverrideRuntimeConfigForTesting(Config);
	FMeteorBurstId Burst;
	const double ImpactTime = Harness.World->GetTimeSeconds() + 1.0;
	if (!TestTrue(TEXT("排程紧预算撞击"), Harness.Meteor->ScheduleStrike(FVector::ZeroVector, ImpactTime, Burst))) return false;
	int32 PreparedLanes = 0;
	const auto PreparedHandle = Harness.Meteor->OnTrajectoryPagePrepared().AddLambda(
		[&](const FMeteorTrajectoryPage& Page) { PreparedLanes += Page.Num(); });
	// 只推进异步预演，不跨过撞击时间；第一批 Ready 后仍有大量候选尚未预演。
	for (int32 I = 0; I < 500 && PreparedLanes == 0; ++I)
	{
		Harness.Advance(0.0001f);
	}
	TestTrue(TEXT("撞击前已有可激活的轨迹"), PreparedLanes > 0 && PreparedLanes < 4096 * 3);
	TestTrue(TEXT("撞击前仍有密集候选积压"), Harness.Meteor->GetAuthorityStats().PendingTargets >= 4096);
	// UWorld 会钳制过大的 DeltaSeconds；逐帧推进到真实撞击帧，不能用一次大步冒充。
	while (Harness.World->GetTimeSeconds() < ImpactTime) Harness.Advance(1.0f / 60.0f);
	Harness.Meteor->OnTrajectoryPagePrepared().Remove(PreparedHandle);
	TestTrue(TEXT("撞击时预演仍未完成"), PreparedLanes < 4096 * 3);
	const auto Stats = Harness.Meteor->GetAuthorityStats();
	TestTrue(TEXT("预演队列未清空也必须在撞击帧提交已就绪源"), Stats.ImpactFrameDestroyedTargets > 0);
	TestTrue(TEXT("同一帧发出碎片激活"), Stats.FirstActivationLaneCount > 0);
	Harness.Storage->UnregisterResidencySource(Residency);
	return true;
}

#endif
