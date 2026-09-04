#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Abilities/AxeSwingGameplayAbility.h"
#include "Abilities/EquipmentAbilityBridgeComponent.h"
#include "Abilities/DemolitionToolSwingGameplayAbility.h"
#include "Abilities/MeteorStrikeGameplayAbility.h"
#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Attributes/ElementCharacterAttributeSet.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Chunk/WorldChunkCoordinates.h"
#include "Components/StaticMeshComponent.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Effects/ElementCharacterBurningEffect.h"
#include "Effects/ElementFireDamageExecution.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectDamageFragment.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Equipment/EquipmentComponent.h"
#include "Equipment/EquippedItemActor.h"
#include "Game/ElementSandboxGameMode.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameplayEffect.h"
#include "Inventory/InventoryComponent.h"
#include "Inventory/InventoryTypes.h"
#include "Item/Features/EquippableItemFeature.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Items/AxeItemDefinition.h"
#include "Items/DemolitionToolItemDefinition.h"
#include "Items/MeteorStrikeItemDefinition.h"
#include "Items/StickEquippedItemActor.h"
#include "Items/StickSwingAbilityFeature.h"
#include "MeteorWorldSubsystem.h"
#include "Meteor/MeteorStrikeActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Tags/ElementGameplayTags.h"
#include "TimerManager.h"
#include "Tree/SettlementTreeDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/StickWorldObjectDefinition.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldStorageSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace ElementSandbox::Abilities::Tests
{
	struct FAbilityTestWorld
	{
		FAbilityTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			PlayerState = World->SpawnActor<AElementSandboxPlayerState>();
			Character = World->SpawnActor<AElementSandboxCharacter>();
		}

		~FAbilityTestWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		AElementSandboxPlayerState* PlayerState = nullptr;
		AElementSandboxCharacter* Character = nullptr;
	};

	UItemInstance* MakeAbilityItem(
		UObject* Outer,
		const TSubclassOf<AEquippedItemActor> EquippedActorClass =
			AEquippedItemActor::StaticClass())
	{
		UItemDefinition* Definition = NewObject<UItemDefinition>(Outer);
		UEquippableItemFeature* Equippable = NewObject<UEquippableItemFeature>(Definition);
		Equippable->EquippedActorClass = EquippedActorClass;
		Equippable->AttachmentSocket = NAME_None;
		Definition->FeatureTemplates.Add(Equippable);
		Definition->FeatureTemplates.Add(NewObject<UStickSwingAbilityFeature>(Definition));

		UItemInstance* Item = NewObject<UItemInstance>(Outer);
		return Item->Initialize(Definition) ? Item : nullptr;
	}

	void ApplyInstantAttributeModifier(
		UAbilitySystemComponent& AbilitySystem,
		const FGameplayAttribute Attribute,
		const float Magnitude)
	{
		UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
		Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
		FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
		Modifier.Attribute = Attribute;
		Modifier.ModifierOp = EGameplayModOp::Additive;
		Modifier.ModifierMagnitude = FScalableFloat(Magnitude);
		AbilitySystem.ApplyGameplayEffectToSelf(
			Effect,
			1.0f,
			AbilitySystem.MakeEffectContext());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEquipmentAbilityGrantTest,
	"ElementSandbox.Abilities.EquipmentGrantAndRevoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEquipmentAbilityGrantTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	TestNotNull(TEXT("创建测试 PlayerState"), Harness.PlayerState);
	TestNotNull(TEXT("创建测试 Character"), Harness.Character);
	if (!Harness.PlayerState || !Harness.Character)
	{
		return false;
	}

	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState->GetElementAbilitySystemComponent();
	UEquipmentComponent* Equipment = Harness.Character->GetEquipmentComponent();
	UEquipmentAbilityBridgeComponent* Bridge =
		Harness.Character->FindComponentByClass<UEquipmentAbilityBridgeComponent>();
	TestNotNull(TEXT("PlayerState 持有 ASC"), AbilitySystem);
	TestNotNull(TEXT("Character 持有装备组件"), Equipment);
	TestNotNull(TEXT("Character 持有 Items 到 GAS 桥"), Bridge);
	if (!AbilitySystem || !Equipment || !Bridge)
	{
		return false;
	}

	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	Bridge->InitializeAbilitySystem(AbilitySystem);
	UItemInstance* Item = MakeAbilityItem(Harness.World);
	TestNotNull(TEXT("创建含 Ability Feature 的道具"), Item);
	if (!Item)
	{
		return false;
	}

	const UEquippableItemFeature* Equippable = Item->FindFeature<UEquippableItemFeature>();
	TestTrue(TEXT("服务器装备道具"), Equippable && Equipment->EquipItem(Item, *Equippable));
	TestEqual(TEXT("装备只授予一个 AbilitySpec"), AbilitySystem->GetActivatableAbilities().Num(), 1);
	if (AbilitySystem->GetActivatableAbilities().Num() == 1)
	{
		const FGameplayAbilitySpec& Spec = AbilitySystem->GetActivatableAbilities()[0];
		TestTrue(TEXT("AbilitySpec 保留道具来源"), Spec.SourceObject.Get() == Item);
		TestTrue(
			TEXT("AbilitySpec 使用主要输入 Tag"),
			Spec.GetDynamicSpecSourceTags().HasTagExact(ElementSandboxGameplayTags::Input_Use_Primary));

		AbilitySystem->AbilityInputTagPressed(ElementSandboxGameplayTags::Input_Use_Primary);
		TestTrue(TEXT("主要输入激活本地预测 Ability"), Spec.IsActive());
	}

	TestTrue(TEXT("卸下道具"), Equipment->UnequipItem());
	TestEqual(TEXT("卸下后精确回收 AbilitySpec"), AbilitySystem->GetActivatableAbilities().Num(), 0);

	UItemInstance* DemolitionTool = NewObject<UItemInstance>(Harness.World);
	TestTrue(TEXT("创建真实拆除锤 Ability Item"),
		DemolitionTool->Initialize(GetMutableDefault<UDemolitionToolItemDefinition>()));
	const UEquippableItemFeature* DemolitionEquippable =
		DemolitionTool->FindFeature<UEquippableItemFeature>();
	TestTrue(TEXT("服务器装备拆除锤"),
		DemolitionEquippable && Equipment->EquipItem(DemolitionTool, *DemolitionEquippable));
	TestEqual(TEXT("拆除锤只授予一个独立 AbilitySpec"),
		AbilitySystem->GetActivatableAbilities().Num(), 1);
	if (AbilitySystem->GetActivatableAbilities().Num() == 1)
	{
		const FGameplayAbilitySpec& DemolitionSpec = AbilitySystem->GetActivatableAbilities()[0];
		TestTrue(TEXT("拆除锤没有复用木棍 Ability 类型"),
			DemolitionSpec.Ability
			&& DemolitionSpec.Ability->GetClass()
				== UDemolitionToolSwingGameplayAbility::StaticClass());
		AbilitySystem->AbilityInputTagPressed(ElementSandboxGameplayTags::Input_Use_Primary);
		TestTrue(TEXT("拆除锤主输入激活自身挥击 Ability"), DemolitionSpec.IsActive());
	}
	TestTrue(TEXT("清理拆除锤装备"), Equipment->UnequipItem());

	UItemInstance* Axe = NewObject<UItemInstance>(Harness.World);
	TestTrue(TEXT("创建真实斧头 Ability Item"),
		Axe->Initialize(GetMutableDefault<UAxeItemDefinition>()));
	const UEquippableItemFeature* AxeEquippable =
		Axe->FindFeature<UEquippableItemFeature>();
	TestTrue(TEXT("服务器装备斧头"),
		AxeEquippable && Equipment->EquipItem(Axe, *AxeEquippable));
	TestEqual(TEXT("斧头只授予一个独立 AbilitySpec"),
		AbilitySystem->GetActivatableAbilities().Num(), 1);
	if (AbilitySystem->GetActivatableAbilities().Num() == 1)
	{
		const FGameplayAbilitySpec& AxeSpec = AbilitySystem->GetActivatableAbilities()[0];
		TestTrue(TEXT("斧头不再复用木棍 Fire Ability"),
			AxeSpec.Ability
			&& AxeSpec.Ability->GetClass() == UAxeSwingGameplayAbility::StaticClass());
	}
	TestTrue(TEXT("清理斧头装备"), Equipment->UnequipItem());

	UItemInstance* Meteor = NewObject<UItemInstance>(Harness.World);
	TestTrue(TEXT("创建真实陨石核心 Ability Item"),
		Meteor->Initialize(GetMutableDefault<UMeteorStrikeItemDefinition>()));
	const UEquippableItemFeature* MeteorEquippable =
		Meteor->FindFeature<UEquippableItemFeature>();
	TestTrue(TEXT("服务器装备陨石核心"),
		MeteorEquippable && Equipment->EquipItem(Meteor, *MeteorEquippable));
	TestEqual(TEXT("陨石核心只授予一个独立 AbilitySpec"),
		AbilitySystem->GetActivatableAbilities().Num(), 1);
	if (AbilitySystem->GetActivatableAbilities().Num() == 1)
	{
		const FGameplayAbilitySpec& MeteorSpec = AbilitySystem->GetActivatableAbilities()[0];
		TestTrue(TEXT("陨石核心授予专用服务器权威 Ability"),
			MeteorSpec.Ability
			&& MeteorSpec.Ability->GetClass() == UMeteorStrikeGameplayAbility::StaticClass());
	}
	TestTrue(TEXT("清理陨石核心装备"), Equipment->UnequipItem());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAxeSwingAuthorityImpactTest,
	"ElementSandbox.Abilities.AxeSwingAuthorityImpact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAxeSwingAuthorityImpactTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	UEquipmentComponent* Equipment = Harness.Character
		? Harness.Character->GetEquipmentComponent()
		: nullptr;
	UEquipmentAbilityBridgeComponent* Bridge = Harness.Character
		? Harness.Character->FindComponentByClass<UEquipmentAbilityBridgeComponent>()
		: nullptr;
	UWorldObjectWorldSubsystem* WorldObjects = Harness.World
		? Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	if (!AbilitySystem || !Equipment || !Bridge || !WorldObjects)
	{
		AddError(TEXT("Axe authority impact integration subsystems did not initialize."));
		return false;
	}

	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	Bridge->InitializeAbilitySystem(AbilitySystem);
	Harness.Character->SetActorLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);

	FWorldObjectCreateDesc TreeDesc;
	TreeDesc.Definition = WorldObjects->FindDefinition(
		GetDefault<USettlementTreeDefinition>()->DefinitionId);
	// 树干近表面距离角色约 130cm，处于 300cm Authority Focus 范围内。
	TreeDesc.WorldTransform = FTransform(FVector(350.0, 0.0, 0.0));
	TreeDesc.MotionState = EWorldObjectMotionState::Dormant;
	const FWorldObjectEntityHandle Tree = WorldObjects->CreateEntity(TreeDesc);
	TestTrue(TEXT("在斧头 Authority 射线上创建树木"), Tree.IsSet());

	UItemInstance* Axe = NewObject<UItemInstance>(Harness.World);
	TestTrue(TEXT("创建并初始化真实斧头道具"),
		Axe && Axe->Initialize(GetMutableDefault<UAxeItemDefinition>()));
	const UEquippableItemFeature* Equippable = Axe
		? Axe->FindFeature<UEquippableItemFeature>()
		: nullptr;
	TestTrue(TEXT("装备斧头并授予真实 Ability"),
		Equippable && Equipment->EquipItem(Axe, *Equippable));
	if (!Tree.IsSet() || AbilitySystem->GetActivatableAbilities().Num() != 1)
	{
		return false;
	}

	const FGameplayAbilitySpecHandle AxeAbility =
		AbilitySystem->GetActivatableAbilities()[0].Handle;
	for (int32 Swing = 0; Swing < 4; ++Swing)
	{
		TestTrue(FString::Printf(TEXT("第 %d 次 Authority 挥斧成功激活"), Swing + 1),
			AbilitySystem->TryActivateAbility(AxeAbility));
		// TimerManager 先把本次 Activate 中加入的 Pending Timer 合并，再推进到 Impact/End。
		++GFrameCounter;
		Harness.World->GetTimerManager().Tick(0.0f);
		++GFrameCounter;
		Harness.World->GetTimerManager().Tick(10.0f);
		if (Swing < 3)
		{
			const FWorldObjectDamageFragment* Damage = WorldObjects->GetRegistry()
				.FindFragment<FWorldObjectDamageFragment>(Tree);
			TestTrue(FString::Printf(TEXT("第 %d 次真实 Impact 累计 25 点伤害"), Swing + 1),
				Damage && FMath::IsNearlyEqual(
					Damage->AccumulatedDamage, static_cast<float>((Swing + 1) * 25)));
		}
	}
	TestFalse(TEXT("四次真实 Ability Impact 后树木已 GameplayDestroy"),
		WorldObjects->IsEntityAlive(Tree));

	int32 WoodBlockCount = 0;
	const UWorldObjectDefinition* WoodBlock = GetDefault<UWoodBlockWorldObjectDefinition>();
	const auto Definitions =
		WorldObjects->GetRegistry().GetFragmentPoolView<FWorldObjectDefinitionFragment>();
	for (const FWorldObjectDefinitionFragment& Definition : Definitions.Fragments)
	{
		WoodBlockCount += Definition.Definition.Get() == WoodBlock ? 1 : 0;
	}
	TestTrue(TEXT("真实 Ability 链生成 3–6 个木块"),
		WoodBlockCount >= 3 && WoodBlockCount <= 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeteorStrikeAuthorityAbilityTest,
	"ElementSandbox.Abilities.MeteorStrikeAuthorityExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeteorStrikeAuthorityAbilityTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	UInventoryComponent* Inventory = Harness.PlayerState
		? Harness.PlayerState->GetInventoryComponent()
		: nullptr;
	UEquipmentAbilityBridgeComponent* Bridge = Harness.Character
		? Harness.Character->FindComponentByClass<UEquipmentAbilityBridgeComponent>()
		: nullptr;
	UMeteorWorldSubsystem* Meteor = Harness.World
		? Harness.World->GetSubsystem<UMeteorWorldSubsystem>()
		: nullptr;
	UWorldStorageSubsystem* Storage = Harness.World
		? Harness.World->GetSubsystem<UWorldStorageSubsystem>()
		: nullptr;
	if (!AbilitySystem || !Inventory || !Bridge || !Meteor || !Storage || !Harness.Character)
	{
		AddError(TEXT("Meteor authority Ability integration subsystems did not initialize."));
		return false;
	}

	Harness.Character->SetPlayerState(Harness.PlayerState);
	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	Bridge->InitializeAbilitySystem(AbilitySystem);
	// Authority 只使用角色的平面朝向，不使用相机俯仰做视线求交。
	Harness.Character->SetActorLocationAndRotation(
		FVector(0.0, 0.0, 200.0), FRotator(45.0, 0.0, 0.0));
	FVector FallbackImpact;
	const FVector EmptyAreaViewer(50000000.0, 50000000.0, 200.0);
	TestTrue(TEXT("前方没有带宿主 Resident Chunk 时陨石仍可获得近距离保底落点"),
		Meteor->TryGetMapImpactLocation(
			EmptyAreaViewer, FVector::ForwardVector, FallbackImpact));
	TestTrue(TEXT("保底落点固定在角色正前方首选距离且不依赖加载新 Chunk"),
		FallbackImpact.Equals(
			FVector(
				EmptyAreaViewer.X + Meteor->GetRuntimeConfig().MeteorShowcasePreferredDistance,
				EmptyAreaViewer.Y,
				Meteor->GetRuntimeConfig().GroundPlaneZ),
			UE_KINDA_SMALL_NUMBER));
	const FWorldChunkCoord ShowcaseChunk(1, 0, 0);
	FWorldResidentEntityRegistration ShowcaseRegistration;
	ShowcaseRegistration.EntityId = Storage->AllocateEntityId();
	ShowcaseRegistration.Domain = EWorldEntityDomain::Building;
	ShowcaseRegistration.HomeChunk = ShowcaseChunk;
	ShowcaseRegistration.StateRevision = 1;
	TestTrue(TEXT("注册一处角色前方近距离占用 Chunk"),
		Storage->RegisterResidentEntity(ShowcaseRegistration) == EWorldResidentUpsertResult::Inserted);
	FVector ExpectedImpact;
	TestTrue(TEXT("Ability 触发前可从角色前方 Resident Chunk 解析唯一落点"),
		Meteor->TryGetMapImpactLocation(
			Harness.Character->GetActorLocation(),
			Harness.Character->GetActorForwardVector(),
			ExpectedImpact));
	TestTrue(TEXT("落点位于角色前方约两百米的有宿主 Chunk"),
		ExpectedImpact.Equals(FVector(15000.0, 5000.0, Meteor->GetRuntimeConfig().GroundPlaneZ),
			UE_KINDA_SMALL_NUMBER));
	for (const FWorldChunkCoord DenserChunk : {FWorldChunkCoord(2, 1, 0), FWorldChunkCoord(15, 0, 0)})
	{
		for (int32 Index = 0; Index < 32; ++Index)
		{
			FWorldResidentEntityRegistration DenserRegistration = ShowcaseRegistration;
			DenserRegistration.EntityId = Storage->AllocateEntityId();
			DenserRegistration.HomeChunk = DenserChunk;
			TestTrue(TEXT("模拟晚施放时新加载的更密集区域"),
				Storage->RegisterResidentEntity(DenserRegistration) == EWorldResidentUpsertResult::Inserted);
		}
	}
	FVector LoadedMapImpact;
	TestTrue(TEXT("加载更多近远 Chunk 后仍可选点"), Meteor->TryGetMapImpactLocation(
		Harness.Character->GetActorLocation(), Harness.Character->GetActorForwardVector(), LoadedMapImpact));
	TestTrue(TEXT("更密集的远处和侧前方建筑不会把已有近处落点拉走"),
		LoadedMapImpact.Equals(ExpectedImpact, UE_KINDA_SMALL_NUMBER));
	UMeteorStrikeItemDefinition* Definition =
		GetMutableDefault<UMeteorStrikeItemDefinition>();
	TestTrue(TEXT("快捷栏只获得唯一一枚陨石核心"),
		Inventory->GrantItemToQuickbar(
			Definition, 0, AElementSandboxGameMode::DefaultMeteorQuantity));
	Inventory->SelectQuickbarSlot(0);
	TestEqual(TEXT("快捷栏选择授予一个陨石 AbilitySpec"),
		AbilitySystem->GetActivatableAbilities().Num(), 1);
	if (AbilitySystem->GetActivatableAbilities().Num() != 1)
	{
		return false;
	}

	const FInventorySlotAddress MeteorAddress(EInventoryContainer::Quickbar, 0);
	UItemInstance* SourceItem = Inventory->GetItem(MeteorAddress);
	const UItemStackFeature* Stack = SourceItem
		? SourceItem->FindFeature<UItemStackFeature>()
		: nullptr;
	const FGameplayAbilitySpec& Spec = AbilitySystem->GetActivatableAbilities()[0];
	TestTrue(TEXT("真实快捷栏来源授予陨石专用 Ability"),
		Spec.SourceObject.Get() == SourceItem && Spec.Ability
		&& Spec.Ability->GetClass() == UMeteorStrikeGameplayAbility::StaticClass());
	TestTrue(TEXT("无需地面瞄准，Authority Ability 按角色前方 Resident Chunk 成功激活"),
		AbilitySystem->TryActivateAbility(Spec.Handle));
	TestTrue(TEXT("Ability 成功创建唯一活动 Burst"), Meteor->HasActiveBurst());
	TestTrue(TEXT("唯一陨石排程后核心被完整消耗"),
		Stack && Inventory->GetItem(MeteorAddress) == nullptr);

	int32 MeteorActorCount = 0;
	AMeteorStrikeActor* MeteorActor = nullptr;
	for (TActorIterator<AMeteorStrikeActor> It(Harness.World); It; ++It)
	{
		MeteorActor = *It;
		++MeteorActorCount;
	}
	TestEqual(TEXT("整张地图只生成一颗主陨石 Actor"), MeteorActorCount, 1);
	const UE::ElementSandbox::Meteor::FMeteorRuntimeConfig RuntimeConfig =
		Meteor->GetRuntimeConfig();
	const FVector ExpectedStart = RuntimeConfig.ComputeMeteorStartLocation(
		ExpectedImpact, Harness.Character->GetActorLocation());
	TestTrue(TEXT("主陨石从角色前方落点的外侧斜向切入而不是头顶垂落"),
		MeteorActor && MeteorActor->GetActorLocation().Equals(ExpectedStart, UE_KINDA_SMALL_NUMBER)
		&& FMath::IsNearlyEqual(
			FVector::Dist2D(ExpectedStart, ExpectedImpact),
			RuntimeConfig.MeteorApproachHorizontalDistance,
			1.0));
	const UStaticMeshComponent* MeteorMesh = MeteorActor
		? MeteorActor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	TestEqual(TEXT("主陨石默认直径为 3 公里"),
		Meteor->GetRuntimeConfig().MeteorDiameter, 300000.0f);
	const float ExpectedUniformScale = Meteor->GetRuntimeConfig().MeteorDiameter / 100.0f;
	TestTrue(TEXT("单颗主陨石使用约 3 公里配置直径"), MeteorMesh
		&& MeteorMesh->GetRelativeScale3D().Equals(FVector(ExpectedUniformScale), UE_KINDA_SMALL_NUMBER));
	TestEqual(TEXT("冲击波只扬起落点周围六公里，不扩展世界加载范围"),
		Meteor->GetRuntimeConfig().ShockwaveRadius, 600000.0f);
	TestEqual(TEXT("撞击瞬间先发布两公里核心"),
		Meteor->GetRuntimeConfig().ImpactCoreRadius, 200000.0f);
	TestEqual(TEXT("外围波前以每秒三公里快速扫过"),
		Meteor->GetRuntimeConfig().ShockwaveSpeed, 300000.0f);
	TestTrue(TEXT("默认解析碎片使用可追踪的三层电影式散弹幕"),
		Meteor->GetRuntimeConfig().DebrisSpeedRange.X >= 9000.0f
		&& Meteor->GetRuntimeConfig().DebrisSpeedRange.Y <= 22000.0f
		&& Meteor->GetRuntimeConfig().DebrisGroundScatterFraction <= 0.20f
		&& Meteor->GetRuntimeConfig().DebrisMediumArcFraction
			+ Meteor->GetRuntimeConfig().DebrisHighArcFraction >= 0.55f
		&& Meteor->GetRuntimeConfig().DebrisLowElevationDegrees.X >= 5.0f
		&& Meteor->GetRuntimeConfig().DebrisHighElevationDegrees.Y >= 70.0f
		&& Meteor->GetRuntimeConfig().GravityZ >= -1800.0f
		&& Meteor->GetRuntimeConfig().DebrisAirMaximumAzimuthDeviationDegrees >= 85.0f
		&& Meteor->GetRuntimeConfig().DebrisAirMaximumAzimuthDeviationDegrees < 90.0f);
	TestTrue(TEXT("单颗主陨石跨地图始终网络相关"), MeteorActor && MeteorActor->bAlwaysRelevant);

	++GFrameCounter;
	Harness.World->GetTimerManager().Tick(0.0f);
	++GFrameCounter;
	Harness.World->GetTimerManager().Tick(10.0f);
	TestFalse(TEXT("挥动表现计时结束后 Ability 正常退出"), Spec.IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStickSwingFireInteractionLifecycleTest,
	"ElementSandbox.Abilities.StickSwingFireInteractionLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStickSwingFireInteractionLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	UEquipmentComponent* Equipment = Harness.Character
		? Harness.Character->GetEquipmentComponent()
		: nullptr;
	UEquipmentAbilityBridgeComponent* AbilityBridge = Harness.Character
		? Harness.Character->FindComponentByClass<UEquipmentAbilityBridgeComponent>()
		: nullptr;
	UWorldObjectWorldSubsystem* WorldObjects = Harness.World
		? Harness.World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	UElementGameplayWorldSubsystem* ElementGameplay = Harness.World
		? Harness.World->GetSubsystem<UElementGameplayWorldSubsystem>()
		: nullptr;
	if (!AbilitySystem || !Equipment || !AbilityBridge || !WorldObjects
		|| !ElementGameplay || !ElementGameplay->IsRuntimeAssemblyActive())
	{
		AddError(TEXT("Stick swing integration subsystems did not initialize."));
		return false;
	}

	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	AbilityBridge->InitializeAbilitySystem(AbilitySystem);
	UItemInstance* Item = MakeAbilityItem(
		Harness.World,
		AStickEquippedItemActor::StaticClass());
	const UEquippableItemFeature* Equippable = Item
		? Item->FindFeature<UEquippableItemFeature>()
		: nullptr;
	TestTrue(TEXT("装备真实木棍 Ability Item"),
		Item && Equippable && Equipment->EquipItem(Item, *Equippable));
	AStickEquippedItemActor* StickActor =
		Cast<AStickEquippedItemActor>(Equipment->GetEquippedActor());
	TestNotNull(TEXT("装备投影为木棍 Actor"), StickActor);
	if (!StickActor)
	{
		return false;
	}

	FWorldObjectCreateDesc Desc;
	Desc.Definition = GetMutableDefault<UStickWorldObjectDefinition>();
	Desc.WorldTransform = StickActor->GetActorTransform();
	Desc.MotionState = EWorldObjectMotionState::Attached;
	Desc.Proxy = StickActor->GetWorldObjectProxyComponent();
	const FWorldObjectEntityHandle Stick = WorldObjects->CreateEntity(Desc);
	TestTrue(TEXT("为装备木棍创建 Attached WorldObject"), Stick.IsSet());
	if (!Stick.IsSet() || AbilitySystem->GetActivatableAbilities().Num() != 1)
	{
		return false;
	}

	const FGameplayAbilitySpecHandle AbilityHandle =
		AbilitySystem->GetActivatableAbilities()[0].Handle;
	TestTrue(TEXT("Authority 激活挥棍 Ability"),
		AbilitySystem->TryActivateAbility(AbilityHandle));
	const FWorldEntityId StickWorldEntityId = WorldObjects->GetWorldEntityId(Stick);
	TestTrue(TEXT("挥棍开始开启 All 广播交互态"),
		ElementGameplay->IsStickFireInteractionActiveForTesting(StickWorldEntityId));

	AbilitySystem->CancelAbilityHandle(AbilityHandle);
	TestTrue(TEXT("取消挥棍恢复 CharacterOnly 广播交互态"),
		!ElementGameplay->IsStickFireInteractionActiveForTesting(StickWorldEntityId));

	TestTrue(TEXT("再次激活用于验证正常结束"),
		AbilitySystem->TryActivateAbility(AbilityHandle));
	++GFrameCounter;
	Harness.World->GetTimerManager().Tick(0.0f);
	++GFrameCounter;
	Harness.World->GetTimerManager().Tick(10.0f);
	TestTrue(TEXT("挥棍计时正常结束同样恢复 CharacterOnly 广播交互态"),
		!ElementGameplay->IsStickFireInteractionActiveForTesting(StickWorldEntityId));

	TestTrue(TEXT("清理装备木棍 WorldObject"), WorldObjects->DestroyEntity(Stick));
	TestTrue(TEXT("清理装备"), Equipment->UnequipItem());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterAttributeDefaultsTest,
	"ElementSandbox.Abilities.CharacterAttributeDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterAttributeDefaultsTest::RunTest(const FString& Parameters)
{
	const UElementCharacterAttributeSet* Attributes = NewObject<UElementCharacterAttributeSet>();
	TestEqual(TEXT("默认生命上限"), Attributes->GetMaxHealth(), 100.0f);
	TestEqual(TEXT("默认生命"), Attributes->GetHealth(), 100.0f);
	TestEqual(TEXT("默认待结算伤害为空"), Attributes->GetIncomingDamage(), 0.0f);
	TestEqual(TEXT("默认耐力上限"), Attributes->GetMaxStamina(), 100.0f);
	TestEqual(TEXT("默认耐力"), Attributes->GetStamina(), 100.0f);
	TestEqual(TEXT("默认火抗"), Attributes->GetFireResistance(), 0.0f);
	TestEqual(TEXT("默认冰抗"), Attributes->GetIceResistance(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterHealthDamagePipelineTest,
	"ElementSandbox.Abilities.CharacterHealthDamagePipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterHealthDamagePipelineTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	const UElementCharacterAttributeSet* Attributes = Harness.PlayerState
		? Harness.PlayerState->GetCharacterAttributes()
		: nullptr;
	if (!AbilitySystem || !Attributes || !Harness.Character)
	{
		AddError(TEXT("Health pipeline test could not create ASC, AttributeSet, and Avatar."));
		return false;
	}

	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	ApplyInstantAttributeModifier(
		*AbilitySystem,
		UElementCharacterAttributeSet::GetIncomingDamageAttribute(),
		30.0f);
	TestEqual(TEXT("IncomingDamage 通过 GAS Effect 扣除生命"), Attributes->GetHealth(), 70.0f);
	TestEqual(TEXT("一次结算后元属性立即清零"), Attributes->GetIncomingDamage(), 0.0f);

	ApplyInstantAttributeModifier(
		*AbilitySystem,
		UElementCharacterAttributeSet::GetIncomingDamageAttribute(),
		200.0f);
	TestEqual(TEXT("过量伤害把生命钳制到零"), Attributes->GetHealth(), 0.0f);
	TestEqual(TEXT("过量伤害同样不会残留元属性"), Attributes->GetIncomingDamage(), 0.0f);

	ApplyInstantAttributeModifier(
		*AbilitySystem,
		UElementCharacterAttributeSet::GetIncomingDamageAttribute(),
		-50.0f);
	TestEqual(TEXT("负伤害不能借道元属性治疗"), Attributes->GetHealth(), 0.0f);
	TestEqual(TEXT("非法负伤害仍被清空"), Attributes->GetIncomingDamage(), 0.0f);

	AbilitySystem->SetNumericAttributeBase(
		UElementCharacterAttributeSet::GetHealthAttribute(),
		100.0f);
	ApplyInstantAttributeModifier(
		*AbilitySystem,
		UElementCharacterAttributeSet::GetMaxHealthAttribute(),
		-60.0f);
	TestEqual(TEXT("生命上限可由 GAS Effect 修改"), Attributes->GetMaxHealth(), 40.0f);
	TestEqual(TEXT("降低生命上限同时钳制当前生命"), Attributes->GetHealth(), 40.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterBurningEffectPolicyTest,
	"ElementSandbox.Abilities.CharacterBurningEffectPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterBurningEffectPolicyTest::RunTest(const FString& Parameters)
{
	const UElementCharacterBurningEffect* Effect =
		GetDefault<UElementCharacterBurningEffect>();
	TestNotNull(TEXT("存在 C++ Burning GameplayEffect"), Effect);
	if (!Effect)
	{
		return false;
	}

	TestTrue(TEXT("燃烧时长只由 Fire ECS 持有，GE 为 Infinite"),
		Effect->DurationPolicy == EGameplayEffectDurationType::Infinite);
	TestTrue(TEXT("DOT 周期为 0.5 秒"),
		FMath::IsNearlyEqual(
			Effect->Period.GetValueAtLevel(1.0f),
			UElementCharacterBurningEffect::PeriodSeconds));
	TestFalse(TEXT("首次接触不立即执行周期伤害"),
		Effect->bExecutePeriodicEffectOnApplication);
	TestEqual(TEXT("同一 Target 最多三个 Burning Stack"),
		Effect->StackLimitCount, 3);
	TestTrue(TEXT("层数变化不承担 ECS 时长刷新"),
		Effect->StackDurationRefreshPolicy
			== EGameplayEffectStackingDurationPolicy::NeverRefresh);
	TestTrue(TEXT("层数变化不重置 0.5 秒 Period"),
		Effect->StackPeriodResetPolicy
			== EGameplayEffectStackingPeriodPolicy::NeverReset);
	TestTrue(TEXT("Burning Effect 授予统一表现 Tag"),
		Effect->GetGrantedTags().HasTagExact(
			ElementSandboxGameplayTags::State_Burning));
	TestTrue(TEXT("周期伤害由火抗平方减伤 Execution 计算"),
		Effect->Modifiers.IsEmpty() && Effect->Executions.Num() == 1
		&& Effect->Executions[0].CalculationClass
			== UElementFireDamageExecution::StaticClass());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterBurningPeriodicDamageTest,
	"ElementSandbox.Abilities.CharacterBurningPeriodicDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterBurningPeriodicDamageTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Abilities::Tests;
	FAbilityTestWorld Harness;
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	const UElementCharacterAttributeSet* Attributes = Harness.PlayerState
		? Harness.PlayerState->GetCharacterAttributes()
		: nullptr;
	if (!AbilitySystem || !Attributes || !Harness.Character)
	{
		AddError(TEXT("Burning periodic damage test could not create its GAS target."));
		return false;
	}

	AbilitySystem->InitAbilityActorInfo(Harness.PlayerState, Harness.Character);
	TestTrue(TEXT("测试冻结 1 damage / 0.5s 规则"),
		UElementCharacterBurningEffect::ConfigureRuntimeRules(1.0f, 0.5f));
	AbilitySystem->SetNumericAttributeBase(
		UElementCharacterAttributeSet::GetFireResistanceAttribute(),
		0.5f);
	const UElementCharacterBurningEffect* Burning =
		GetDefault<UElementCharacterBurningEffect>();
	FActiveGameplayEffectHandle Handle;
	for (int32 Stack = 1; Stack <= 3; ++Stack)
	{
		Handle = AbilitySystem->ApplyGameplayEffectToSelf(
			Burning,
			1.0f,
			AbilitySystem->MakeEffectContext());
		TestEqual(TEXT("重复应用聚合到目标层数"),
			AbilitySystem->GetCurrentStackCount(Handle),
			Stack);
	}
	TestEqual(TEXT("应用时不立即造成周期伤害"), Attributes->GetHealth(), 100.0f);
	TestTrue(TEXT("Burning Tag 由唯一 GE 授予"),
		AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning));

	AbilitySystem->ExecutePeriodicEffectForAutomation(Handle);
	TestTrue(TEXT("三层伤害 = 1 × 3 × (1-0.5)^2"),
		FMath::IsNearlyEqual(Attributes->GetHealth(), 99.25f));
	TestTrue(TEXT("只移除一层而不重建 Effect"),
		AbilitySystem->RemoveActiveGameplayEffect(Handle, 1));
	TestEqual(TEXT("降为两层"), AbilitySystem->GetCurrentStackCount(Handle), 2);
	AbilitySystem->ExecutePeriodicEffectForAutomation(Handle);
	TestTrue(TEXT("两层伤害 = 1 × 2 × (1-0.5)^2"),
		FMath::IsNearlyEqual(Attributes->GetHealth(), 98.75f));
	TestTrue(TEXT("移除剩余 Burning Effect"),
		AbilitySystem->RemoveActiveGameplayEffect(Handle));
	TestFalse(TEXT("GE 移除后 Burning Tag 消失"),
		AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning));
	return true;
}

#endif
