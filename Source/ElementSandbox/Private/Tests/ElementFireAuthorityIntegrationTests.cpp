#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Attributes/ElementCharacterAttributeSet.h"
#include "BuildingCatalogWorldSubsystem.h"
#include "BuildingWorldSubsystem.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Chunk/WorldChunkTypes.h"
#include "City/CityBuildingPieceDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Definition/BuildingDefinition.h"
#include "ElementGameplayWorldSubsystem.h"
#include "ElementSimulationSubsystem.h"
#include "Elements/Fire/FireballProjectile.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildRenderCustomDataFragment.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectDamageFragment.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformProcess.h"
#include "Items/StickEquippedItemActor.h"
#include "Misc/AutomationTest.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "Tags/ElementGameplayTags.h"
#include "Tree/SettlementTreeDefinition.h"
#include "Torch/TorchDefinition.h"
#include "Visual/ElementVisualJournal.h"
#include "Wood/WoodBuildingDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/CharcoalWorldObjectDefinition.h"
#include "WorldObjects/StickWorldObjectDefinition.h"
#include "WorldStorageSubsystem.h"

namespace ElementSandbox::Fire::AuthorityIntegration::Tests
{
	struct FFireTestWorld final
	{
		explicit FFireTestWorld(
			const FName WorldName = TEXT("ElementFireAuthorityIntegration"),
			const ENetMode InitialNetMode = NM_Standalone)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true)
				.ShouldSimulatePhysics(false)
				.EnableTraceCollision(true)
				.CreateNavigation(false)
				.CreateAISystem(false);
			World = UWorld::CreateWorld(
				EWorldType::PIE,
				false,
				WorldName,
				nullptr,
				true,
				ERHIFeatureLevel::Num,
				&Values,
				true);
			if (!World) return;
			GEngine->CreateNewWorldContext(EWorldType::PIE).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(InitialNetMode);
			World->InitWorld(Values);
			World->UpdateWorldComponents(true, false);
			Storage = World->GetSubsystem<UWorldStorageSubsystem>();
			Buildings = World->GetSubsystem<UBuildingWorldSubsystem>();
			BuildingCatalog = World->GetSubsystem<UBuildingCatalogWorldSubsystem>();
			WorldObjects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			ElementGameplay = World->GetSubsystem<UElementGameplayWorldSubsystem>();
			Simulation = World->GetSubsystem<UElementSimulationSubsystem>();
		}

		~FFireTestWorld()
		{
			if (!World) return;
			for (const FWorldResidencySourceHandle Source : ResidencySources)
			{
				if (Storage && Source.IsSet()) Storage->UnregisterResidencySource(Source);
			}
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		bool IsValid() const
		{
			return World && Storage && Buildings && BuildingCatalog && WorldObjects && ElementGameplay
				&& ElementGameplay->IsRuntimeAssemblyActive() && Simulation;
		}

		bool AddResidencySource(const FVector& Location)
		{
			const FWorldResidencySourceHandle Source = Storage->RegisterResidencySource(Location);
			if (!Source.IsSet()) return false;
			ResidencySources.Add(Source);
			return true;
		}

		bool RemoveAllResidencySources()
		{
			bool bSucceeded = true;
			for (const FWorldResidencySourceHandle Source : ResidencySources)
			{
				bSucceeded = Storage->UnregisterResidencySource(Source) && bSucceeded;
			}
			ResidencySources.Reset();
			return bSucceeded;
		}

		void BeginPlay() const { World->BeginPlay(); }

		void TickStep() const
		{
			++GFrameCounter;
			World->Tick(LEVELTICK_All, 0.125f);
		}

		bool TickUntil(const TFunctionRef<bool()> Predicate, const int64 DeadlineMilliseconds) const
		{
			while (Storage->GetWorldSimulationTimeMilliseconds() < DeadlineMilliseconds && !Predicate())
			{
				TickStep();
			}
			return Predicate();
		}

		bool TickUntilAsync(const TFunctionRef<bool()> Predicate, const int32 MaximumSteps) const
		{
			for (int32 Step = 0; Step < MaximumSteps && !Predicate(); ++Step)
			{
				TickStep();
				FPlatformProcess::SleepNoStats(0.001f);
			}
			return Predicate();
		}

		void TickThrough(const int64 TimeMilliseconds) const
		{
			while (Storage->GetWorldSimulationTimeMilliseconds() < TimeMilliseconds) TickStep();
		}

		UWorld* World = nullptr;
		UWorldStorageSubsystem* Storage = nullptr;
		UBuildingWorldSubsystem* Buildings = nullptr;
		UBuildingCatalogWorldSubsystem* BuildingCatalog = nullptr;
		UWorldObjectWorldSubsystem* WorldObjects = nullptr;
		UElementGameplayWorldSubsystem* ElementGameplay = nullptr;
		UElementSimulationSubsystem* Simulation = nullptr;
		TArray<FWorldResidencySourceHandle> ResidencySources;
	};

	struct FRegisteredCharacter final
	{
		AElementSandboxPlayerState* PlayerState = nullptr;
		AElementSandboxPlayerController* Controller = nullptr;
		AElementSandboxCharacter* Character = nullptr;
		UElementAbilitySystemComponent* AbilitySystem = nullptr;

		bool IsValid() const
		{
			return PlayerState && Controller && Character && AbilitySystem;
		}
	};

	FRegisteredCharacter AddCharacter(FFireTestWorld& Harness, const FVector& Location)
	{
		FRegisteredCharacter Result;
		Result.PlayerState = Harness.World->SpawnActor<AElementSandboxPlayerState>();
		Result.Controller = Harness.World->SpawnActor<AElementSandboxPlayerController>();
		Result.Character = Harness.World->SpawnActor<AElementSandboxCharacter>(
			Location, FRotator::ZeroRotator);
		Result.AbilitySystem = Result.PlayerState
			? Result.PlayerState->GetElementAbilitySystemComponent() : nullptr;
		if (Result.Controller && Result.PlayerState && Result.Character)
		{
			Result.Controller->SetPlayerState(Result.PlayerState);
			Result.Controller->Possess(Result.Character);
		}
		return Result;
	}

	float ReadBuildingBurnAmount(const FFireTestWorld& Harness, const FBuildEntityHandle Entity)
	{
		const FBuildRenderCustomDataFragment* Custom = Harness.Buildings->GetRegistry()
			.FindFragment<FBuildRenderCustomDataFragment>(Entity);
		return Custom && Custom->Values.IsValidIndex(0) ? Custom->Values[0] : 0.0f;
	}

	bool TryGetBuildingQueryBounds(
		const FFireTestWorld& Harness,
		const FBuildEntityHandle Entity,
		FBox& OutBounds)
	{
		OutBounds = FBox(ForceInit);
		for (int32 Offset = 0;;)
		{
			FBuildQuerySnapshotPage Page;
			if (!Harness.Buildings->CopyQuerySnapshotPage(Offset, 128, Page)) return false;
			for (const FBuildShapeInstanceSnapshot& Shape : Page.Shapes)
			{
				if (Shape.ShapeRef.Entity == Entity) OutBounds += Shape.WorldBounds;
			}
			if (!Page.bHasMore) break;
			Offset = Page.NextOffset;
		}
		return OutBounds.IsValid != 0;
	}

	int32 CountFireVisuals(const FFireTestWorld& Harness, const FWorldEntityId WorldEntityId)
	{
		int32 Count = 0;
		const TSharedPtr<FElementVisualJournal, ESPMode::ThreadSafe> Journal =
			Harness.Simulation->GetVisualJournal();
		if (!Journal) return Count;
		TArray<FElementVisualShardKey> Shards;
		Journal->GetKnownShards(Shards);
		for (const FElementVisualShardKey Shard : Shards)
		{
			FElementVisualShardSnapshot Snapshot;
			if (!Journal->CopyShardSnapshot(Shard, Snapshot)) continue;
			for (const FElementVisualDescriptor& Descriptor : Snapshot.GetDescriptors())
			{
				if (Descriptor.Key.GetWorldEntityId() == WorldEntityId
					&& Descriptor.Key.GetVisualKind() == TEXT("Fire.Flame"))
				{
					++Count;
				}
			}
		}
		return Count;
	}

	int32 GatherCharcoal(
		const FFireTestWorld& Harness,
		TArray<FWorldObjectEntityHandle>& OutEntities)
	{
		OutEntities.Reset();
		const UWorldObjectDefinition* Charcoal = GetDefault<UCharcoalWorldObjectDefinition>();
		const auto Definitions = Harness.WorldObjects->GetRegistry()
			.GetFragmentPoolView<FWorldObjectDefinitionFragment>();
		for (int32 Index = 0; Index < Definitions.Num(); ++Index)
		{
			if (Definitions.Fragments[Index].Definition.Get() == Charcoal)
			{
				OutEntities.Add(Definitions.Entities[Index]);
			}
		}
		return OutEntities.Num();
	}

	struct FCapturedRecords final
	{
		void Attach(UWorldStorageSubsystem& InStorage)
		{
			Storage = &InStorage;
			Handle = Storage->OnAuthorityMutation().AddRaw(this, &FCapturedRecords::OnMutation);
		}

		void Detach()
		{
			if (Storage && Handle.IsValid()) Storage->OnAuthorityMutation().Remove(Handle);
			Handle.Reset();
			Storage = nullptr;
		}

		void OnMutation(const FWorldStorageEntityMutation& Mutation)
		{
			if (!Storage || Mutation.Kind == EWorldStorageMutationKind::GameplayTombstone) return;
			FWorldPersistentEntityRecord Record;
			FString Error;
			if (!Storage->CaptureResidentRecord(Mutation.EntityId, Record, Error)) return;
			LatestByDomain.Add(Record.Domain, MoveTemp(Record));
		}

		const FWorldPersistentEntityRecord* Find(const EWorldEntityDomain Domain) const
		{
			return LatestByDomain.Find(Domain);
		}

		UWorldStorageSubsystem* Storage = nullptr;
		FDelegateHandle Handle;
		TMap<EWorldEntityDomain, FWorldPersistentEntityRecord> LatestByDomain;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireBurnoutCharcoalConversionTest,
	"ElementSandbox.Element.Fire.Integration.BurnoutConvertsBuildingAndTreeToCharcoal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireBurnoutCharcoalConversionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireBurnoutCharcoal"));
	if (!TestTrue(TEXT("Burnout World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("Burnout 建立 Residency"), Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	UWoodBuildingDefinition* PillarDefinition = NewObject<UWoodBuildingDefinition>(Harness.World);
	const bool bBuildingDefinitionReady = PillarDefinition
		&& PillarDefinition->Initialize(
			TEXT("Test.Fire.Burnout.Building"), FVector(20.0, 20.0, 300.0))
		&& Harness.Buildings->RegisterDefinition(*PillarDefinition);
	USettlementTreeDefinition* TreeDefinition = Cast<USettlementTreeDefinition>(
		Harness.WorldObjects->FindDefinition(GetDefault<USettlementTreeDefinition>()->DefinitionId));
	const FBuildEntityHandle Pillar = bBuildingDefinitionReady
		? Harness.Buildings->CreateEntity(*PillarDefinition, FTransform(FVector::ZeroVector))
		: FBuildEntityHandle();
	FWorldObjectCreateDesc TreeDesc;
	TreeDesc.Definition = TreeDefinition;
	TreeDesc.WorldTransform = FTransform(FVector(1000.0, 0.0, 0.0));
	TreeDesc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle Tree = TreeDefinition
		? Harness.WorldObjects->CreateEntity(TreeDesc)
		: FWorldObjectEntityHandle();
	const FWorldEntityId PillarId = Harness.Buildings->GetWorldEntityId(Pillar);
	const FWorldEntityId TreeId = Harness.WorldObjects->GetWorldEntityId(Tree);
	bool bCreatedTargets = true;
	bCreatedTargets &= TestTrue(TEXT("初始化并注册可燃 Building Definition"), bBuildingDefinitionReady);
	bCreatedTargets &= TestNotNull(TEXT("生产 Settlement.Tree Definition 已注册"), TreeDefinition);
	bCreatedTargets &= TestTrue(TEXT("创建可燃 Building"), Pillar.IsSet() && PillarId.IsSet());
	bCreatedTargets &= TestTrue(TEXT("创建可燃树"), Tree.IsSet() && TreeId.IsSet());
	if (!bCreatedTargets)
	{
		return false;
	}
	Harness.BeginPlay();

	bool bBuildingHadDamage = false;
	bool bTreeHadDamage = false;
	const FDelegateHandle BuildingPreDestroy = Harness.Buildings->OnEntityPreDestroy().AddLambda(
		[&](const FBuildEntityHandle Entity, bool&)
		{
			if (Entity == Pillar)
			{
				bBuildingHadDamage = Harness.Buildings->GetRegistry()
					.HasFragment<FBuildDamageFragment>(Entity);
			}
		});
	const FDelegateHandle TreePreDestroy = Harness.WorldObjects->OnEntityPreDestroy().AddLambda(
		[&](const FWorldObjectEntityHandle Entity, bool&)
		{
			if (Entity == Tree)
			{
				bTreeHadDamage = Harness.WorldObjects->GetRegistry()
					.HasFragment<FWorldObjectDamageFragment>(Entity);
			}
		});

	const FElementRuntimeFireSourceHandle BuildingFire =
		Harness.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	const FElementRuntimeFireSourceHandle TreeFire =
		Harness.ElementGameplay->CreateFireballSource(FVector(1000.0, 0.0, 100.0));
	TestTrue(TEXT("Building 与树进入 BurnedOut 后都完成源→木炭转换"),
		BuildingFire.IsSet() && TreeFire.IsSet() && Harness.TickUntil(
			[&]()
			{
				return !Harness.Buildings->IsEntityAlive(Pillar)
					&& !Harness.WorldObjects->IsEntityAlive(Tree);
			},
			Harness.Storage->GetWorldSimulationTimeMilliseconds() + 25000));
	Harness.Buildings->OnEntityPreDestroy().Remove(BuildingPreDestroy);
	Harness.WorldObjects->OnEntityPreDestroy().Remove(TreePreDestroy);
	TestFalse(TEXT("燃尽转换不为 Building 分配斧头伤害 Fragment"), bBuildingHadDamage);
	TestFalse(TEXT("燃尽转换不为树分配斧头伤害 Fragment"), bTreeHadDamage);
	TestEqual(TEXT("源火焰表现随 GameplayDestroy 清理"),
		CountFireVisuals(Harness, PillarId) + CountFireVisuals(Harness, TreeId), 0);

	TArray<FWorldObjectEntityHandle> Charcoal;
	const int32 ProductCount = GatherCharcoal(Harness, Charcoal);
	TestTrue(TEXT("两个燃尽宿主各生成 3–6 块木炭"),
		ProductCount >= 6 && ProductCount <= 12);
	for (const FWorldObjectEntityHandle Product : Charcoal)
	{
		const FWorldObjectMotionFragment* Motion = Harness.WorldObjects->GetRegistry()
			.FindFragment<FWorldObjectMotionFragment>(Product);
		const FWorldObjectPhysicsBodyFragment* Physics = Harness.WorldObjects->GetRegistry()
			.FindFragment<FWorldObjectPhysicsBodyFragment>(Product);
		TestTrue(TEXT("木炭复用 Physics + LooseDebris 产品生命周期"),
			Motion && Motion->State == EWorldObjectMotionState::Physics
				&& Physics
				&& Physics->CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireBurnoutCharcoalRollbackTest,
	"ElementSandbox.Element.Fire.Integration.BurnoutCharcoalRollbackAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireBurnoutCharcoalRollbackTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireBurnoutCharcoalRollback"));
	if (!TestTrue(TEXT("Burnout Rollback World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("Burnout Rollback 建立 Residency"),
			Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	UWoodBuildingDefinition* PillarDefinition = NewObject<UWoodBuildingDefinition>(Harness.World);
	const bool bBuildingDefinitionReady = PillarDefinition
		&& PillarDefinition->Initialize(
			TEXT("Test.Fire.Burnout.RollbackBuilding"), FVector(20.0, 20.0, 300.0))
		&& Harness.Buildings->RegisterDefinition(*PillarDefinition);
	const FBuildEntityHandle Pillar = bBuildingDefinitionReady
		? Harness.Buildings->CreateEntity(*PillarDefinition, FTransform(FVector::ZeroVector))
		: FBuildEntityHandle();
	if (!TestTrue(TEXT("初始化并注册回滚测试 Building Definition"), bBuildingDefinitionReady)
		|| !TestTrue(TEXT("创建回滚测试 Building"), Pillar.IsSet()))
	{
		return false;
	}
	Harness.BeginPlay();

	bool bRejectDestroy = true;
	bool bObservedStagedCharcoal = false;
	bool bObservedDamageFragment = false;
	const FDelegateHandle RejectHandle = Harness.Buildings->OnEntityPreDestroy().AddLambda(
		[&](const FBuildEntityHandle Entity, bool& bCanDestroy)
		{
			if (Entity != Pillar)
			{
				return;
			}
			TArray<FWorldObjectEntityHandle> Staged;
			bObservedStagedCharcoal |= GatherCharcoal(Harness, Staged) >= 3;
			bObservedDamageFragment |= Harness.Buildings->GetRegistry()
				.HasFragment<FBuildDamageFragment>(Entity);
			if (bRejectDestroy)
			{
				bCanDestroy = false;
			}
		});

	const FElementRuntimeFireSourceHandle Fire =
		Harness.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	TestTrue(TEXT("源销毁被否决时 Building 保持 BurnedOut"),
		Fire.IsSet() && Harness.TickUntil(
			[&]()
			{
				return Harness.Buildings->IsEntityAlive(Pillar)
					&& ReadBuildingBurnAmount(Harness, Pillar) >= 1.0f;
			},
			Harness.Storage->GetWorldSimulationTimeMilliseconds() + 25000));
	TestTrue(TEXT("源销毁前已经 Stage 全批木炭"), bObservedStagedCharcoal);
	TArray<FWorldObjectEntityHandle> Charcoal;
	TestEqual(TEXT("销毁否决后不留下未发布木炭"), GatherCharcoal(Harness, Charcoal), 0);
	TestFalse(TEXT("燃尽重试始终不创建 Damage Fragment"), bObservedDamageFragment);

	bRejectDestroy = false;
	TestTrue(TEXT("解除否决后下个 Authority Step 复用同一请求完成转换"),
		Harness.TickUntil(
			[&]() { return !Harness.Buildings->IsEntityAlive(Pillar); },
			Harness.Storage->GetWorldSimulationTimeMilliseconds() + 2000));
	Harness.Buildings->OnEntityPreDestroy().Remove(RejectHandle);
	const int32 ProductCount = GatherCharcoal(Harness, Charcoal);
	TestTrue(TEXT("重试成功后只留下 3–6 块木炭"),
		ProductCount >= 3 && ProductCount <= 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireAuthorityProjectionTest,
	"ElementSandbox.Element.Fire.Integration.AuthorityProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireAuthorityProjectionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness;
	if (!TestTrue(TEXT("Fire Authority World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("建立原点 Residency"), Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	Harness.BeginPlay();

	UBuildingDefinition* PillarDefinition = Harness.Buildings->FindDefinition(TEXT("WoodPillar"));
	UStickWorldObjectDefinition* StickDefinition = Cast<UStickWorldObjectDefinition>(
		Harness.WorldObjects->FindDefinition(TEXT("Stick")));
	const FBuildEntityHandle Pillar = PillarDefinition
		? Harness.Buildings->CreateEntity(*PillarDefinition, FTransform(FVector::ZeroVector))
		: FBuildEntityHandle();
	const FWorldEntityId PillarId = Harness.Buildings->GetWorldEntityId(Pillar);
	const FRegisteredCharacter Character = AddCharacter(Harness, FVector(0.0, 0.0, 82.0));

	AStickEquippedItemActor* StickActor = Harness.World->SpawnActor<AStickEquippedItemActor>(
		FVector(0.0, 0.0, 82.0), FRotator::ZeroRotator);
	FWorldObjectEntityHandle Stick;
	if (StickActor && StickDefinition)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = StickDefinition;
		Desc.WorldTransform = StickActor->GetActorTransform();
		Desc.MotionState = EWorldObjectMotionState::Physics;
		Desc.Proxy = StickActor->GetWorldObjectProxyComponent();
		Stick = Harness.WorldObjects->CreateEntity(Desc);
	}
	if (!TestTrue(TEXT("创建 Building、Character 与 Stick 三类目标"),
		Pillar.IsSet() && PillarId.IsSet() && Character.IsValid() && Stick.IsSet()))
	{
		return false;
	}

	const FElementRuntimeFireSourceHandle Fireball =
		Harness.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	const auto AllProjected = [&]()
	{
		return ReadBuildingBurnAmount(Harness, Pillar) > 0.01f
			&& Character.AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning)
			&& StickActor->IsBurning();
	};
	TestTrue(TEXT("同一集中查询批次向三种宿主投影已提交 Burning"),
		Fireball.IsSet() && Harness.TickUntil(
			AllProjected, Harness.Storage->GetWorldSimulationTimeMilliseconds() + 4000));
	TestEqual(TEXT("Building 复合 Shape 只产生一个逻辑火焰表现"),
		CountFireVisuals(Harness, PillarId), 1);
	const float InitialBurnAmount = ReadBuildingBurnAmount(Harness, Pillar);
	Harness.TickThrough(Harness.Storage->GetWorldSimulationTimeMilliseconds() + 2000);
	const float LaterBurnAmount = ReadBuildingBurnAmount(Harness, Pillar);
	TestTrue(TEXT("Burning 期间 BurnAmount 随时间持续增加"),
		LaterBurnAmount > InitialBurnAmount + 0.05f);
	TestTrue(TEXT("未到 BurnEnd 不会提前全黑"), LaterBurnAmount < 1.0f);
	const FElementAuthorityExecution* Execution = Harness.Simulation->GetAuthorityExecution();
	TestTrue(TEXT("Authority 运行集中查询、Numeric 与 State 两阶段"),
		Execution && Execution->GetStats().BvhCandidateCount > 0
			&& Execution->GetStats().NumericProcessorInvocationCount > 0
			&& Execution->GetStats().StateProcessorInvocationCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireCharacterMoveThenFootImpactTest,
	"ElementSandbox.Element.Fire.Integration.CharacterMoveThenFootImpactBurnsAndDamages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireCharacterMoveThenFootImpactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireCharacterMoveThenFootImpact"));
	if (!TestTrue(TEXT("Character Fire 回归 World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("Character Fire 回归建立 Residency"),
			Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	Harness.BeginPlay();

	const FRegisteredCharacter Character = AddCharacter(Harness, FVector(0.0, 0.0, 96.0));
	UCharacterQuerySnapshotSubsystem* CharacterSnapshots =
		Harness.World->GetSubsystem<UCharacterQuerySnapshotSubsystem>();
	UCharacterMovementComponent* Movement = Character.Character
		? Character.Character->GetCharacterMovement() : nullptr;
	if (!TestTrue(TEXT("创建并注册 Character Target"),
		Character.IsValid() && CharacterSnapshots && Movement))
	{
		return false;
	}
	Movement->DisableMovement();

	const FCharacterSnapshotHandle CharacterHandle =
		CharacterSnapshots->FindSnapshot(*Character.Character);
	FElementTargetKey Target;
	Target.Domain = EElementTargetDomain::Character;
	Target.RegistryId = CharacterHandle.GetRegistryId();
	Target.Slot = CharacterHandle.GetSlot();
	Target.Generation = CharacterHandle.GetGeneration();
	const FElementAuthorityExecution* Execution = Harness.Simulation->GetAuthorityExecution();
	FElementAuthorityTargetStateSnapshot ColdState;
	const bool bColdCommitted = CharacterHandle.IsSet() && Execution
		&& Harness.TickUntil(
			[&]()
			{
				return Execution->CaptureTargetState(Target, ColdState)
					&& !ColdState.StateValues.IsEmpty();
			},
			Harness.Storage->GetWorldSimulationTimeMilliseconds() + 1000);
	if (!TestTrue(TEXT("Character 先提交稳定 Cold Thermal State"), bColdCommitted))
	{
		return false;
	}

	const int64 ColdSettlementTime = ColdState.LastSettlementMilliseconds;
	const uint64 ColdThermalRevision = ColdState.StateValues[0].Revision;
	if (!TestTrue(TEXT("在点火前移动 Character"), Character.Character->SetActorLocation(
		Character.Character->GetActorLocation() + FVector(25.0, 0.0, 0.0),
		false, nullptr, ETeleportType::TeleportPhysics)))
	{
		return false;
	}
	FElementAuthorityTargetStateSnapshot MovedState;
	const bool bMotionSettled = Harness.TickUntil(
		[&]()
		{
			return Execution->CaptureTargetState(Target, MovedState)
				&& MovedState.LastSettlementMilliseconds > ColdSettlementTime;
		},
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 1000);
	if (!TestTrue(TEXT("移动快照经 Authority Barrier 完成结算"), bMotionSettled)
		|| !TestTrue(TEXT("移动后 Thermal State 与稳定结算时钟同步推进"),
			!MovedState.StateValues.IsEmpty()
				&& MovedState.StateValues[0].Revision > ColdThermalRevision))
	{
		return false;
	}

	const float HealthBeforeImpact = Character.AbilitySystem->GetNumericAttribute(
		UElementCharacterAttributeSet::GetHealthAttribute());
	const FVector FootLocation = Character.Character->GetActorLocation()
		- FVector(0.0, 0.0, Character.Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFireballProjectile* Projectile = Harness.World->SpawnActor<AFireballProjectile>(
		AFireballProjectile::StaticClass(), FTransform(FootLocation), SpawnParameters);
	const bool bImpactCreated = Projectile
		&& Projectile->ImpactAtLocation(FootLocation, FVector::UpVector)
		&& Projectile->GetImpactFireSource().IsSet();
	if (!TestTrue(TEXT("脚底真实 Fireball Impact 创建 Runtime Fire Source"), bImpactCreated))
	{
		return false;
	}

	const bool bBurning = Harness.TickUntil(
		[&]()
		{
			return Character.AbilitySystem->HasMatchingGameplayTag(
				ElementSandboxGameplayTags::State_Burning);
		},
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 4000);
	TestTrue(TEXT("移动过的 Character 仍可被脚底火焰点燃"), bBurning);
	TestTrue(TEXT("Burning 周期伤害实际扣除 Health"), bBurning && Harness.TickUntil(
		[&]()
		{
			return Character.AbilitySystem->GetNumericAttribute(
				UElementCharacterAttributeSet::GetHealthAttribute()) < HealthBeforeImpact;
		},
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 2000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireRuntimeEvictRestoreTest,
	"ElementSandbox.Element.Fire.Integration.RuntimeEvictRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireRuntimeEvictRestoreTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireRuntimeEvictRestore"));
	if (!TestTrue(TEXT("RuntimeEvict World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("建立原点 Residency"), Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	FCapturedRecords Captured;
	Captured.Attach(*Harness.Storage);
	Harness.BeginPlay();

	UBuildingDefinition* PillarDefinition = Harness.Buildings->FindDefinition(TEXT("WoodPillar"));
	const FBuildEntityHandle Pillar = PillarDefinition
		? Harness.Buildings->CreateEntity(*PillarDefinition, FTransform(FVector::ZeroVector))
		: FBuildEntityHandle();
	const FWorldEntityId PillarId = Harness.Buildings->GetWorldEntityId(Pillar);
	const uint32 OldGeneration = Pillar.GetGeneration();
	const FElementRuntimeFireSourceHandle Fireball =
		Harness.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	const bool bBurning = Pillar.IsSet() && Fireball.IsSet() && Harness.TickUntil(
		[&](){ return ReadBuildingBurnAmount(Harness, Pillar) > 0.01f; },
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 4000);
	const FWorldPersistentEntityRecord* ElementRecord = Captured.Find(EWorldEntityDomain::Element);
	TestTrue(TEXT("非默认 Thermal 状态独立登记 Element 世界实体"),
		bBurning && ElementRecord && ElementRecord->EntityId != PillarId
			&& ElementRecord->Payload.Num() >= 4
			&& ElementRecord->Payload[0] == 'E' && ElementRecord->Payload[1] == 'L'
			&& ElementRecord->Payload[2] == 'M' && ElementRecord->Payload[3] == '1');
	Harness.ElementGameplay->RemoveRuntimeFireSource(Fireball);

	TestTrue(TEXT("移除 Residency 来源"), Harness.RemoveAllResidencySources());
	TestTrue(TEXT("Host 与 Dependent Element 完成 Capture 后 RuntimeEvict"), Harness.TickUntil(
		[&](){ return !Harness.Buildings->IsEntityAlive(Pillar); },
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 5000));
	Harness.TickThrough(Harness.Storage->GetWorldSimulationTimeMilliseconds() + 2000);
	TestTrue(TEXT("重新建立 Residency"), Harness.AddResidencySource(FVector::ZeroVector));
	TestTrue(TEXT("Primary Host 恢复后 Dependent Element 原子恢复"), Harness.TickUntilAsync(
		[&](){ return Harness.Buildings->FindEntity(PillarId).IsSet(); }, 128));
	Harness.TickStep();
	const FBuildEntityHandle Restored = Harness.Buildings->FindEntity(PillarId);
	TestTrue(TEXT("Restore 保留 WorldEntityId、换新运行 Generation 并重建投影"),
		Restored.IsSet() && Restored.GetGeneration() != OldGeneration
			&& ReadBuildingBurnAmount(Harness, Restored) > 0.0f);
	Captured.Detach();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireAuthorityToClientTest,
	"ElementSandbox.Element.Fire.Integration.AuthorityToClientProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireAuthorityToClientTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Authority(TEXT("ElementFireAuthorityProjection"), NM_Standalone);
	FFireTestWorld Client(TEXT("ElementFireClientProjection"), NM_Client);
	if (!TestTrue(TEXT("Authority 与 Client World 装配成功"), Authority.IsValid() && Client.IsValid())
		|| !TestTrue(TEXT("Authority 建立 Residency"), Authority.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	FCapturedRecords Captured;
	Captured.Attach(*Authority.Storage);
	Authority.BeginPlay();
	Client.BeginPlay();
	TestNotNull(TEXT("Authority 创建执行核"), Authority.Simulation->GetAuthorityExecution());
	TestNull(TEXT("Client 不创建 BVH、查询或 Processor"), Client.Simulation->GetAuthorityExecution());

	UBuildingDefinition* PillarDefinition = Authority.Buildings->FindDefinition(TEXT("WoodPillar"));
	const FBuildEntityHandle Pillar = PillarDefinition
		? Authority.Buildings->CreateEntity(*PillarDefinition, FTransform(FVector::ZeroVector))
		: FBuildEntityHandle();
	const FWorldEntityId PillarId = Authority.Buildings->GetWorldEntityId(Pillar);
	const FElementRuntimeFireSourceHandle Fireball =
		Authority.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	TestTrue(TEXT("Authority 产生已提交 Building Burning"),
		Pillar.IsSet() && Fireball.IsSet() && Authority.TickUntil(
			[&](){ return ReadBuildingBurnAmount(Authority, Pillar) > 0.01f; },
			Authority.Storage->GetWorldSimulationTimeMilliseconds() + 4000));

	const FWorldPersistentEntityRecord* HostRecord = Captured.Find(EWorldEntityDomain::Building);
	const FWorldPersistentEntityRecord* ElementRecord = Captured.Find(EWorldEntityDomain::Element);
	if (!TestTrue(TEXT("网络记录把 Host 与 Element 状态拆为两个领域"),
		HostRecord && ElementRecord && HostRecord->EntityId == PillarId
			&& ElementRecord->EntityId != PillarId))
	{
		Captured.Detach();
		return false;
	}
	TestTrue(TEXT("Client 先恢复 Primary Host"), Client.Storage->ApplyNetworkUpsert(*HostRecord));
	TestTrue(TEXT("Client 再恢复 Dependent Element 并只做表现投影"),
		Client.Storage->ApplyNetworkUpsert(*ElementRecord));
	const FBuildEntityHandle ClientPillar = Client.Buildings->FindEntity(PillarId);
	Client.TickStep();
	TestTrue(TEXT("Client 从服务器 Element 状态重建 Building 烧黑"),
		ClientPillar.IsSet() && ReadBuildingBurnAmount(Client, ClientPillar) > 0.0f);
	TestEqual(TEXT("Client 从服务器状态重建一个火焰 Visual"),
		CountFireVisuals(Client, PillarId), 1);
	Captured.Detach();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireballBuildingImpactIgnitionTest,
	"ElementSandbox.Element.Fire.Integration.FireballOffCenterImpactIgnitesProjectedBuilding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireballBuildingImpactIgnitionTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireballBuildingCollision"));
	if (!TestTrue(TEXT("Fireball Collision World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("Fireball Collision 建立 Residency"),
			Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	Harness.BeginPlay();
	UBuildingDefinition* WallDefinition = Harness.Buildings->FindDefinition(TEXT("WoodWall"));
	const FBuildEntityHandle Wall = WallDefinition
		? Harness.Buildings->CreateEntity(
			*WallDefinition, FTransform(FVector(1200.0, 0.0, 0.0)))
		: FBuildEntityHandle();
	if (!TestTrue(TEXT("创建真实木墙"), WallDefinition && Wall.IsSet())) return false;

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFireballProjectile* Projectile = Harness.World->SpawnActor<AFireballProjectile>(
		AFireballProjectile::StaticClass(),
		// 故意射向墙面偏心位置：落点离整体中心足够远，旧的 AABB 中心取样无法点燃。
		FTransform(FVector(0.0, 170.0, 250.0)),
		SpawnParameters);
	const int32 SourcesBeforeLaunch = Harness.Buildings->GetCollisionSourceCount();
	if (!TestTrue(TEXT("Authority Fireball 注册 Building Collision Source"),
		Projectile && Projectile->LaunchAuthority(FVector::ForwardVector, FPredictionKey())
			&& Harness.Buildings->GetCollisionSourceCount() == SourcesBeforeLaunch + 1))
	{
		return false;
	}
	Projectile->SetActorTickEnabled(false);
	for (int32 Step = 0; Step < 12 && IsValid(Projectile) && !Projectile->HasImpacted(); ++Step)
	{
		Projectile->Tick(0.125f);
		Harness.TickStep();
	}
	if (!TestTrue(TEXT("Fireball 在木墙偏心表面产生权威命中"),
		IsValid(Projectile) && Projectile->HasImpacted())) return false;
	TestTrue(TEXT("Fireball 未穿到木墙背面"), Projectile->GetActorLocation().X < 1200.0);
	TestTrue(TEXT("偏心落点离墙体 AABB 中心超过旧方案的点燃距离"),
		FVector::Distance(Projectile->GetActorLocation(), FVector(1200.0, 0.0, 150.0)) > 130.0);
	TestTrue(TEXT("命中已创建 RuntimeOnly Fire Source"), Projectile->GetImpactFireSource().IsSet());
	TestEqual(TEXT("命中后立即注销 Collision Source"),
		Harness.Buildings->GetCollisionSourceCount(), SourcesBeforeLaunch);
	TestTrue(TEXT("Fireball 偏心直接命中使木墙进入 Burning"), Harness.TickUntil(
		[&](){ return ReadBuildingBurnAmount(Harness, Wall) > 0.01f; },
		Harness.Storage->GetWorldSimulationTimeMilliseconds() + 4000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementMountedTorchExternalFireBurnoutTest,
	"ElementSandbox.Element.Fire.Integration.MountedTorchExternalFireBurnsOutWithoutSelfIgnition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementMountedTorchExternalFireBurnoutTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementMountedTorchExternalFireBurnout"));
	if (!TestTrue(TEXT("可燃挂墙火炬 Fire World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("可燃挂墙火炬建立 Residency"),
			Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	UBuildingDefinition* TorchDefinition =
		Harness.Buildings->FindDefinition(GetMountedTorchBuildingDefinitionId());
	const FBuildEntityHandle Torch = TorchDefinition
		? Harness.Buildings->CreateEntity(*TorchDefinition, FTransform::Identity)
		: FBuildEntityHandle();
	const FWorldEntityId TorchId = Harness.Buildings->GetWorldEntityId(Torch);
	if (!TestTrue(TEXT("创建同时具有固定火源和可燃 Target 的挂墙火炬"),
		TorchDefinition && Torch.IsSet() && TorchId.IsSet()))
	{
		return false;
	}
	Harness.BeginPlay();

	const int64 SelfBurnDeadline = Harness.Storage->GetWorldSimulationTimeMilliseconds() + 5000;
	Harness.TickThrough(SelfBurnDeadline);
	TestTrue(TEXT("火炬自己的 CharacterOnly 固定火源不会烧毁自身"),
		Harness.Buildings->IsEntityAlive(Torch)
			&& ReadBuildingBurnAmount(Harness, Torch) <= 0.01f);
	const FElementAuthorityExecution* Execution = Harness.Simulation->GetAuthorityExecution();
	TestEqual(TEXT("未受外火时只有火炬自身固定火源"),
		Execution ? Execution->GetStats().InfluenceCount : 0, 1);

	const FElementRuntimeFireSourceHandle ExternalFire =
		Harness.ElementGameplay->CreateFireballSource(FVector(-24.0, 0.0, 92.0));
	const int64 ExternalFireStart = Harness.Storage->GetWorldSimulationTimeMilliseconds();
	TestTrue(TEXT("外部 All 火源可使挂墙火炬进入 Burning"),
		ExternalFire.IsSet() && Harness.TickUntil(
			[&]() { return ReadBuildingBurnAmount(Harness, Torch) > 0.01f; },
			ExternalFireStart + 4000));
	const float InitialBurnAmount = ReadBuildingBurnAmount(Harness, Torch);
	Harness.TickThrough(Harness.Storage->GetWorldSimulationTimeMilliseconds() + 2000);
	TestTrue(TEXT("挂墙火炬木杆沿用普通 Building 连续烧黑"),
		ReadBuildingBurnAmount(Harness, Torch) > InitialBurnAmount + 0.05f);

	TestTrue(TEXT("挂墙火炬燃尽后复用普通 Building 木炭转换"), Harness.TickUntil(
		[&]() { return !Harness.Buildings->IsEntityAlive(Torch); },
		ExternalFireStart + 25000));
	Harness.TickStep();
	TestEqual(TEXT("火炬销毁同时清理固定、外部与燃烧生成火源"),
		Execution ? Execution->GetStats().InfluenceCount : 0, 0);
	TestEqual(TEXT("火炬源火焰表现随 GameplayDestroy 清理"),
		CountFireVisuals(Harness, TorchId), 0);
	TArray<FWorldObjectEntityHandle> Charcoal;
	const int32 ProductCount = GatherCharcoal(Harness, Charcoal);
	TestTrue(TEXT("火炬燃尽生成 3–6 块统一木炭产品"),
		ProductCount >= 3 && ProductCount <= 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireLongCityPiecePropagationTest,
	"ElementSandbox.Element.Fire.Integration.LongCityPiecePropagatesBySurfaceDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireLongCityPiecePropagationTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Fire::AuthorityIntegration::Tests;
	FFireTestWorld Harness(TEXT("ElementFireLongCityPiecePropagation"));
	if (!TestTrue(TEXT("长城市部件 Fire World 装配成功"), Harness.IsValid())
		|| !TestTrue(TEXT("长城市部件建立 Residency"),
			Harness.AddResidencySource(FVector::ZeroVector)))
	{
		return false;
	}
	Harness.BeginPlay();

	UCityBuildingPieceDefinition* Definition = Harness.BuildingCatalog->GetCityBuildingPieceDefinition(
		ECityBuildingPieceKind::SolidBox, TEXT("Surface.City.Wood"));
	if (!TestNotNull(TEXT("获取真实可燃城市 SolidBox Definition"), Definition)) return false;
	const FVector BeamScale(22.0, 0.4, 0.4);
	const FBuildEntityHandle First = Harness.Buildings->CreateEntity(
		*Definition,
		FTransform(FQuat::Identity, FVector(0.0, 0.0, 100.0), BeamScale));
	FBox FirstBounds(ForceInit);
	if (!TestTrue(TEXT("创建第一根 22m 城市木梁并取得 Element 查询边界"),
		First.IsSet() && TryGetBuildingQueryBounds(Harness, First, FirstBounds)))
	{
		return false;
	}
	constexpr double SurfaceGapCentimeters = 20.0;
	const double BeamSpacing = FirstBounds.GetSize().X + SurfaceGapCentimeters;
	auto CreateBeam = [&](const double X)
	{
		return Harness.Buildings->CreateEntity(
			*Definition,
			FTransform(FQuat::Identity, FVector(X, 0.0, 100.0), BeamScale));
	};
	const FBuildEntityHandle Second = CreateBeam(BeamSpacing);
	const FBuildEntityHandle Third = CreateBeam(BeamSpacing * 2.0);
	if (!TestTrue(TEXT("创建三根端部间隔 20cm 的 22m 城市木梁"),
		First.IsSet() && Second.IsSet() && Third.IsSet()))
	{
		return false;
	}

	const FElementRuntimeFireSourceHandle Fireball =
		Harness.ElementGameplay->CreateFireballSource(FVector(0.0, 0.0, 100.0));
	const int64 StartTime = Harness.Storage->GetWorldSimulationTimeMilliseconds();
	const bool bReachedFirst = Fireball.IsSet() && Harness.TickUntil(
		[&]() { return ReadBuildingBurnAmount(Harness, First) > 0.01f; }, StartTime + 4000);
	TestTrue(TEXT("第一根长梁被直接命中的 Fireball 可靠点燃"), bReachedFirst);
	TestTrue(TEXT("Fireball 不会同时点燃相邻长梁"),
		ReadBuildingBurnAmount(Harness, Second) <= 0.01f
			&& ReadBuildingBurnAmount(Harness, Third) <= 0.01f);

	Harness.TickThrough(StartTime + 4000);
	TestTrue(TEXT("木结构传播不会在数秒内瞬间铺满"),
		ReadBuildingBurnAmount(Harness, Second) <= 0.01f
			&& ReadBuildingBurnAmount(Harness, Third) <= 0.01f);
	const bool bReachedSecond = Harness.TickUntil(
		[&]() { return ReadBuildingBurnAmount(Harness, Second) > 0.01f; }, StartTime + 15000);
	TestTrue(TEXT("火焰先从第一根逐层传到第二根"), bReachedSecond);
	TestTrue(TEXT("第二根刚点燃时第三根仍未着火"), ReadBuildingBurnAmount(Harness, Third) <= 0.01f);
	const bool bReachedThird = Harness.TickUntil(
		[&]() { return ReadBuildingBurnAmount(Harness, Third) > 0.01f; }, StartTime + 26000);
	TestTrue(TEXT("火焰按表面距离通过两次 20cm 间隔传到第三根长梁"), bReachedThird);
	TestTrue(TEXT("较晚着火的构件不会与首块同步燃尽"),
		Harness.Buildings->IsEntityAlive(Second) && Harness.Buildings->IsEntityAlive(Third));
	TestTrue(TEXT("中间长梁已进入持续烧黑"), ReadBuildingBurnAmount(Harness, Second) > 0.01f);
	return true;
}

#endif
