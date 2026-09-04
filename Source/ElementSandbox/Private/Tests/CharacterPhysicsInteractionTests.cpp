#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ElementSandboxCharacter.h"
#include "Characters/ElementSandboxCharacterMovementComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Entity/WorldObjectPhysicsTypes.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterPhysicsInteractionMassTuningTest,
	"ElementSandbox.Character.PhysicsInteraction.MassSensitivePushTuning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterPhysicsInteractionMassTuningTest::RunTest(const FString& Parameters)
{
	const AElementSandboxCharacter* Character = GetDefault<AElementSandboxCharacter>();
	const UCharacterMovementComponent* Movement = Character
		? Character->GetCharacterMovement()
		: nullptr;
	if (!TestNotNull(TEXT("角色默认对象提供 CharacterMovement"), Movement))
	{
		return false;
	}

	TestTrue(TEXT("角色保留与物理对象的 Gameplay 互动"),
		Movement->bEnablePhysicsInteraction);
	TestTrue(TEXT("角色使用可限制 LooseDebris 推速的专用 Movement"),
		Movement->IsA<UElementSandboxCharacterMovementComponent>());
	TestTrue(TEXT("可动落脚面不会把自身旋转传给角色与镜头"),
		Movement->bIgnoreBaseRotation);
	TestFalse(TEXT("推力不按刚体质量放大，质量能够降低加速度"),
		Movement->bPushForceScaledToMass);
	TestTrue(TEXT("推力落点移到刚体中部，避免侧碰产生过大翻转扭矩"),
		Movement->bPushForceUsingZOffset
		&& FMath::IsNearlyZero(Movement->PushForcePointZOffsetFactor));
	TestEqual(TEXT("静止刚体首次接触推力保持克制"),
		Movement->InitialPushForceFactor, 500.0f);
	TestEqual(TEXT("持续推力不使用 UE 默认的 750000"),
		Movement->PushForceFactor, 10000.0f);

	constexpr float RepresentativePieceMassKg = 20.0f;
	constexpr float FrameSeconds = 1.0f / 60.0f;
	const float ContinuousVelocityDelta =
		Movement->PushForceFactor / RepresentativePieceMassKg * FrameSeconds;
	const float InitialVelocityDelta =
		Movement->InitialPushForceFactor * 30.0f
		/ RepresentativePieceMassKg * FrameSeconds;
	TestTrue(TEXT("20kg 对象单帧持续推力速度增量低于 10cm/s"),
		ContinuousVelocityDelta < 10.0f);
	TestTrue(TEXT("20kg 静止对象首次接触速度增量低于 15cm/s"),
		InitialVelocityDelta < 15.0f);
	const FVector LightDebrisVelocity =
		UE::ElementSandbox::WorldObjects::Physics::ComputePawnPushVelocity(
			FVector(500.0f, 0.0f, 100.0f), 4.0f);
	const FVector HeavyDebrisVelocity =
		UE::ElementSandbox::WorldObjects::Physics::ComputePawnPushVelocity(
			FVector(500.0f, 0.0f, 100.0f), 40.0f);
	TestTrue(TEXT("LooseDebris 单次水平踢速不超过 6m/s 且不产生向上分量"),
		LightDebrisVelocity.Size2D() <= 600.0f && FMath::IsNearlyZero(LightDebrisVelocity.Z));
	TestTrue(TEXT("更重的 LooseDebris 获得更低的让路速度"),
		HeavyDebrisVelocity.Size2D() < LightDebrisVelocity.Size2D());
	AddInfo(FString::Printf(
		TEXT("CharacterPhysicsPush: Mass=%.1fkg InitialDelta=%.2fcm/s ContinuousDelta=%.2fcm/s"),
		RepresentativePieceMassKg,
		InitialVelocityDelta,
		ContinuousVelocityDelta));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCharacterPhysicsBaseRotationTest,
	"ElementSandbox.Character.PhysicsInteraction.RotatingBaseKeepsCharacterFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterPhysicsBaseRotationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("CharacterPhysicsBaseRotation"),
		nullptr,
		true);
	if (!TestNotNull(TEXT("创建 Based Movement 测试世界"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);

	AElementSandboxCharacter* Character = World->SpawnActor<AElementSandboxCharacter>(
		FVector(100.0f, 0.0f, 100.0f),
		FRotator(0.0f, 15.0f, 0.0f));
	AActor* BaseActor = World->SpawnActor<AActor>();
	UBoxComponent* BaseBox = BaseActor
		? NewObject<UBoxComponent>(BaseActor, TEXT("RotatingPhysicsBase"))
		: nullptr;
	if (BaseActor && BaseBox)
	{
		BaseActor->AddInstanceComponent(BaseBox);
		BaseActor->SetRootComponent(BaseBox);
		BaseBox->SetMobility(EComponentMobility::Movable);
			BaseBox->SetBoxExtent(FVector(50.0f, 50.0f, 10.0f));
			BaseBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			BaseBox->SetCastShadow(false);
			BaseBox->RegisterComponent();
	}

	UCharacterMovementComponent* Movement = Character
		? Character->GetCharacterMovement()
		: nullptr;
	const bool bSetupValid = TestTrue(TEXT("创建角色与可动落脚面"),
		Character && Movement && BaseActor && BaseBox);
	if (bSetupValid)
	{
		Movement->SetBase(BaseBox);
		Movement->SaveBaseLocation();
		const FQuat FacingBefore = Character->GetActorQuat();
		BaseActor->SetActorRotation(
			FRotator(0.0f, 90.0f, 0.0f),
			ETeleportType::TeleportPhysics);
		Movement->UpdateBasedMovement(1.0f / 60.0f);

		TestTrue(TEXT("旋转后仍保留同一可动 Movement Base"),
			Character->GetMovementBase() == BaseBox);
		TestTrue(TEXT("可动 Base 旋转不会改变角色朝向"),
			Character->GetActorQuat().Equals(FacingBefore, UE_KINDA_SMALL_NUMBER));
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bSetupValid;
}

#endif
