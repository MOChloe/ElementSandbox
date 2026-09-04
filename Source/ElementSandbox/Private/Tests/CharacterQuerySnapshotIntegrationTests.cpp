#if WITH_DEV_AUTOMATION_TESTS

#include "CharacterQuerySnapshotSubsystem.h"
#include "CharacterQuerySnapshotTypes.h"
#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementSandboxCharacterQuerySnapshotLifecycleTest,
	"ElementSandbox.Characters.Integration.QuerySnapshotLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementSandboxCharacterQuerySnapshotLifecycleTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("CharacterQuerySnapshotIntegration"),
		nullptr,
		true);
	check(World);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);

	UCharacterQuerySnapshotSubsystem* CharacterSubsystem =
		World->GetSubsystem<UCharacterQuerySnapshotSubsystem>();
	AElementSandboxPlayerState* PlayerState =
		World->SpawnActor<AElementSandboxPlayerState>();
	AElementSandboxPlayerController* Controller =
		World->SpawnActor<AElementSandboxPlayerController>();
	AElementSandboxCharacter* Character =
		World->SpawnActor<AElementSandboxCharacter>();
	TestNotNull(TEXT("Game World 自动创建 Character 查询快照服务"), CharacterSubsystem);
	TestNotNull(TEXT("创建项目 Character"), Character);
	if (!CharacterSubsystem || !PlayerState || !Controller || !Character)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	Controller->SetPlayerState(PlayerState);
	Controller->Possess(Character);
	const FCharacterSnapshotHandle SnapshotHandle = CharacterSubsystem->FindSnapshot(*Character);
	FCharacterQuerySnapshot Snapshot;
	TestTrue(TEXT("PossessedBy 初始化 ASC 后注册 Character 查询快照"), SnapshotHandle.IsSet());
	TestTrue(TEXT("查询快照包含有效 Capsule POD"),
		CharacterSubsystem->CopySnapshot(SnapshotHandle, Snapshot) && Snapshot.Capsule.IsValid());
	TestTrue(TEXT("Game Thread 解析表可解析普通 Character Actor"),
		CharacterSubsystem->ResolveCharacter(SnapshotHandle) == Character);
	TestTrue(TEXT("Game Thread 解析表可解析 ASC"),
		CharacterSubsystem->ResolveAbilitySystem(SnapshotHandle)
			== PlayerState->GetElementAbilitySystemComponent());
	FCharacterQuerySnapshotBatch MotionBatch;
	const FDelegateHandle MotionHandle = CharacterSubsystem->OnSnapshotsCommitted().AddLambda(
		[&MotionBatch](const FCharacterQuerySnapshotBatch& Batch)
		{
			for (const FCharacterQuerySnapshotChange& Change : Batch.Changes)
			{
				if (Change.Kind == ECharacterQuerySnapshotChangeKind::Motion)
				{
					MotionBatch = Batch;
					break;
				}
			}
		});
	Character->SetActorLocation(FVector(250.0, 0.0, 0.0));
	CharacterSubsystem->EnsurePostActorSnapshotsCurrent();
	TestEqual(TEXT("Post-Actor 一次发布一份角色运动变化"), MotionBatch.Changes.Num(), 1);
	if (!MotionBatch.Changes.IsEmpty())
	{
		const FCharacterQuerySnapshotChange& Change = MotionBatch.Changes[0];
		TestTrue(TEXT("运动变化同时携带前后 Capsule POD"),
			Change.Previous.IsSet() && Change.Current.IsSet()
			&& Change.Current->WorldTransform.GetLocation().Equals(FVector(250.0, 0.0, 0.0)));
	}
	const uint64 PassesBeforeRepeat = CharacterSubsystem->GetStats().TotalPostActorPassCount;
	CharacterSubsystem->EnsurePostActorSnapshotsCurrent();
	TestEqual(TEXT("同一引擎帧不会重复采样角色数组"),
		CharacterSubsystem->GetStats().TotalPostActorPassCount, PassesBeforeRepeat);
	CharacterSubsystem->OnSnapshotsCommitted().Remove(MotionHandle);

	Controller->UnPossess();
	TestFalse(TEXT("UnPossessed 在清除 ASC ActorInfo 前注销 Character 查询快照"),
		CharacterSubsystem->CopySnapshot(SnapshotHandle, Snapshot));
	TestFalse(TEXT("注销后 Actor 不再解析为查询快照"),
		CharacterSubsystem->FindSnapshot(*Character).IsSet());

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
