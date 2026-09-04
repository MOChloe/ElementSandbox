#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Attributes/ElementCharacterAttributeSet.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "CharacterQuerySnapshotTypes.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Effects/ElementCharacterBurningEffect.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Game/ElementSandboxGameMode.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "GameplayEffect.h"
#include "Inventory/InventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Tags/ElementGameplayTags.h"

namespace ElementSandbox::Characters::DeathTests
{
	struct FDeathTestWorld final
	{
		explicit FDeathTestWorld(const bool bCreatePlayerStart)
		{
			GameInstance = NewObject<UGameInstance>(GEngine);
			World = UWorld::CreateWorld(
				EWorldType::Game,
				false,
				TEXT("CharacterDeathIntegration"),
				nullptr,
				true);
			check(GameInstance && World);

			FWorldContext& WorldContext =
				GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.OwningGameInstance = GameInstance;
			WorldContext.SetCurrentWorld(World);
			World->SetGameInstance(GameInstance);
			GameInstance->Init();
			World->SetGameMode(FURL());

			GameMode = World->GetAuthGameMode<AElementSandboxGameMode>();
			if (bCreatePlayerStart)
			{
				SpawnPlayerStart();
			}
			PlayerState = World->SpawnActor<AElementSandboxPlayerState>();
			Controller = World->SpawnActor<AElementSandboxPlayerController>();
			Character = World->SpawnActor<AElementSandboxCharacter>(
				FVector::ZeroVector, FRotator::ZeroRotator);
			if (PlayerState && Controller && Character)
			{
				Controller->SetPlayerState(PlayerState);
				Controller->Possess(Character);
			}
		}

		~FDeathTestWorld()
		{
			if (World)
			{
				if (GameInstance)
				{
					GameInstance->Shutdown();
				}
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		APlayerStart* SpawnPlayerStart() const
		{
			return World
				? World->SpawnActor<APlayerStart>(
					FVector(600.0, -200.0, 100.0),
					FRotator(0.0, 35.0, 0.0))
				: nullptr;
		}

		void ApplyDamage(const float Damage) const
		{
			UElementAbilitySystemComponent* AbilitySystem = PlayerState
				? PlayerState->GetElementAbilitySystemComponent()
				: nullptr;
			if (!AbilitySystem)
			{
				return;
			}
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = UElementCharacterAttributeSet::GetIncomingDamageAttribute();
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(Damage);
			AbilitySystem->ApplyGameplayEffectToSelf(
				Effect, 1.0f, AbilitySystem->MakeEffectContext());
		}

		void AddTemporaryEffect() const
		{
			UElementAbilitySystemComponent* AbilitySystem = PlayerState
				? PlayerState->GetElementAbilitySystemComponent()
				: nullptr;
			if (!AbilitySystem)
			{
				return;
			}
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
			Effect->DurationPolicy = EGameplayEffectDurationType::HasDuration;
			Effect->DurationMagnitude = FScalableFloat(30.0f);
			AbilitySystem->ApplyGameplayEffectToSelf(
				Effect, 1.0f, AbilitySystem->MakeEffectContext());
		}

			FActiveGameplayEffectHandle AddPersistentEffect() const
		{
			UElementAbilitySystemComponent* AbilitySystem = PlayerState
				? PlayerState->GetElementAbilitySystemComponent()
				: nullptr;
			if (!AbilitySystem)
			{
				return FActiveGameplayEffectHandle();
			}
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
			Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;
				return AbilitySystem->ApplyGameplayEffectToSelf(
					Effect, 1.0f, AbilitySystem->MakeEffectContext());
			}

			FActiveGameplayEffectHandle AddBurningEffect() const
			{
				UElementAbilitySystemComponent* AbilitySystem = PlayerState
					? PlayerState->GetElementAbilitySystemComponent()
					: nullptr;
				return AbilitySystem
					? AbilitySystem->ApplyGameplayEffectToSelf(
						GetDefault<UElementCharacterBurningEffect>(),
						1.0f,
						AbilitySystem->MakeEffectContext())
					: FActiveGameplayEffectHandle();
			}

		void Tick() const
		{
			++GFrameCounter;
			World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		}

		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		AElementSandboxGameMode* GameMode = nullptr;
		AElementSandboxPlayerState* PlayerState = nullptr;
		AElementSandboxPlayerController* Controller = nullptr;
		AElementSandboxCharacter* Character = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterDeathRespawnLifecycleTest,
	"ElementSandbox.Characters.Death.RespawnLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterDeathRespawnLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Characters::DeathTests;
	FDeathTestWorld Harness(true);
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	UCharacterQuerySnapshotSubsystem* Characters = Harness.World
		? Harness.World->GetSubsystem<UCharacterQuerySnapshotSubsystem>()
			: nullptr;
		UElementGameplayWorldSubsystem* ElementGameplay = Harness.World
			? Harness.World->GetSubsystem<UElementGameplayWorldSubsystem>()
			: nullptr;
		if (!Harness.GameMode || !Harness.PlayerState || !Harness.Controller
			|| !Harness.Character || !AbilitySystem || !Characters
			|| !ElementGameplay || !ElementGameplay->IsRuntimeAssemblyActive())
	{
		AddError(TEXT("Death test world failed to initialize."));
		return false;
	}

	APawn* InitialPawn = Harness.Controller->GetPawn();
	const FCharacterSnapshotHandle InitialSnapshot =
		Characters->FindSnapshot(*Harness.Character);
	UInventoryComponent* PersistentInventory =
		Harness.PlayerState->GetInventoryComponent();
	Harness.Controller->RequestRespawn();
	TestTrue(TEXT("存活时 R 不替换 Pawn"),
		Harness.Controller->GetPawn() == InitialPawn);

	AbilitySystem->SetNumericAttributeBase(
		UElementCharacterAttributeSet::GetStaminaAttribute(), 25.0f);
		const FActiveGameplayEffectHandle PersistentEffect =
			Harness.AddPersistentEffect();
		Harness.AddTemporaryEffect();
		const FActiveGameplayEffectHandle BurningEffect = Harness.AddBurningEffect();
		TestEqual(TEXT("死亡前同时存在普通持久、临时与 Element Burning Effect"),
			AbilitySystem->GetActiveEffects(FGameplayEffectQuery()).Num(), 3);
		TestTrue(TEXT("死亡前 Burning Effect 提供 State.Burning"),
			AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning));
	Harness.ApplyDamage(500.0f);
	TestEqual(TEXT("过量伤害把 Health 钳制为零"),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetHealthAttribute()), 0.0f);
	Harness.Tick();
	TestTrue(TEXT("死亡后仍保留当前 Pawn 维持镜头"),
		Harness.Controller->GetPawn() == InitialPawn);
		TestTrue(TEXT("Authority 在下一 Tick 冻结 CharacterMovement"),
			Harness.Character->GetCharacterMovement()->MovementMode == MOVE_None);
		TestNull(TEXT("死亡当刻精确移除 Element Burning Effect"),
			AbilitySystem->GetActiveGameplayEffect(BurningEffect));
		TestFalse(TEXT("死亡镜头占位 Pawn 不再保留 State.Burning"),
			AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning));
		TestEqual(TEXT("死亡封口不移除普通 Infinite 与临时 Effect"),
			AbilitySystem->GetActiveEffects(FGameplayEffectQuery()).Num(), 2);

	Harness.Controller->RequestRespawn();
	AElementSandboxCharacter* RespawnedCharacter =
		Cast<AElementSandboxCharacter>(Harness.Controller->GetPawn());
	TestTrue(TEXT("R 在 PlayerStart 创建新的 Character Pawn"),
		RespawnedCharacter && RespawnedCharacter != InitialPawn);
	TestFalse(TEXT("旧 Pawn 已销毁"), IsValid(InitialPawn));
	TestEqual(TEXT("重生恢复满 Health"),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetHealthAttribute()),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetMaxHealthAttribute()));
	TestEqual(TEXT("重生恢复满 Stamina"),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetStaminaAttribute()),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetMaxStaminaAttribute()));
	TestEqual(TEXT("重生只清理临时 GameplayEffect"),
		AbilitySystem->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
		TestNotNull(TEXT("跨 Pawn 的持久 GameplayEffect 保留"),
			AbilitySystem->GetActiveGameplayEffect(PersistentEffect));
		TestFalse(TEXT("新 Pawn 不继承旧 Character Fire Host 的 State.Burning"),
			AbilitySystem->HasMatchingGameplayTag(ElementSandboxGameplayTags::State_Burning));
	TestTrue(TEXT("PlayerState ASC 跨 Pawn 保持同一实例"),
		AbilitySystem == Harness.PlayerState->GetElementAbilitySystemComponent());
	TestTrue(TEXT("PlayerState 背包跨 Pawn 保持同一实例"),
		PersistentInventory == Harness.PlayerState->GetInventoryComponent());

	FCharacterQuerySnapshot RemovedSnapshot;
	TestFalse(TEXT("旧 Character 查询快照已注销"),
		Characters->CopySnapshot(InitialSnapshot, RemovedSnapshot));
	const FCharacterSnapshotHandle RespawnedSnapshot = RespawnedCharacter
		? Characters->FindSnapshot(*RespawnedCharacter)
		: FCharacterSnapshotHandle();
	TestTrue(TEXT("新 Avatar 重新注册不同 Generation 的 Character 查询快照"),
		RespawnedSnapshot.IsSet() && RespawnedSnapshot != InitialSnapshot);
	APawn* PawnAfterRespawn = Harness.Controller->GetPawn();
	Harness.Controller->RequestRespawn();
	TestTrue(TEXT("重生后重复按 R 不再替换 Pawn"),
		Harness.Controller->GetPawn() == PawnAfterRespawn);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterRespawnFailureRollbackTest,
	"ElementSandbox.Characters.Death.SpawnFailureKeepsDeathState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterRespawnFailureRollbackTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Characters::DeathTests;
	FDeathTestWorld Harness(false);
	UElementAbilitySystemComponent* AbilitySystem = Harness.PlayerState
		? Harness.PlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	if (!Harness.GameMode || !Harness.Controller || !Harness.Character || !AbilitySystem)
	{
		AddError(TEXT("Respawn failure test world failed to initialize."));
		return false;
	}

	Harness.ApplyDamage(500.0f);
	Harness.Tick();
	Harness.Controller->RequestRespawn();
	TestNull(TEXT("没有 PlayerStart 时不会留下半初始化 Pawn"),
		Harness.Controller->GetPawn());
	TestEqual(TEXT("生成失败把 Health 回滚为零"),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetHealthAttribute()), 0.0f);

	TestNotNull(TEXT("补充 PlayerStart"), Harness.SpawnPlayerStart());
	Harness.Controller->RequestRespawn();
	TestNotNull(TEXT("死亡状态保留，后续 R 可以重试成功"),
		Harness.Controller->GetPawn().Get());
	TestEqual(TEXT("重试成功恢复满 Health"),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetHealthAttribute()),
		AbilitySystem->GetNumericAttribute(
			UElementCharacterAttributeSet::GetMaxHealthAttribute()));
	return true;
}

#endif
