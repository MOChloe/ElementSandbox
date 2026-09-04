#if WITH_DEV_AUTOMATION_TESTS

#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldStorageSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoodBlockCollisionAlignmentTest,
	"ElementSandbox.WorldObjects.WoodBlock.CollisionMatchesVisibleGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoodBlockCollisionAlignmentTest::RunTest(const FString& Parameters)
{
	const auto Values = UWorld::InitializationValues().CreatePhysicsScene(true)
		.ShouldSimulatePhysics(true).EnableTraceCollision(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WoodCollisionAlignment"), nullptr,
		true, ERHIFeatureLevel::Num, &Values, true);
	if (!TestNotNull(TEXT("碰撞测试 World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitWorld(Values);
	World->UpdateWorldComponents(true, false);
	struct FWorldCleanup
	{
		UWorld* World;
		~FWorldCleanup() { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
	} Cleanup{World};
	auto* Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
	auto* Collision = World->GetSubsystem<UWorldObjectCollisionWorldSubsystem>();
	auto* Storage = World->GetSubsystem<UWorldStorageSubsystem>();
	Storage->RegisterResidencySource(FVector::ZeroVector);
	auto* Definition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
	Objects->RegisterDefinition(*Definition);
	auto* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/WorldObjects/WoodBlock/SM_WoodBlock.SM_WoodBlock"));
	if (!TestNotNull(TEXT("使用实际可见木块资产"), Mesh)) return false;
	const FBox VisibleBounds = Mesh->GetBounds().GetBox();

	AActor* Floor = World->SpawnActor<AActor>();
	auto* FloorBox = NewObject<UBoxComponent>(Floor);
	Floor->AddInstanceComponent(FloorBox);
	Floor->SetRootComponent(FloorBox);
	FloorBox->SetBoxExtent(FVector(2000, 1000, 5));
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	FloorBox->RegisterComponent();
	Floor->SetActorLocation(FVector(0, 0, -5));

	TArray<FWorldObjectEntityHandle> Entities;
	for (const double Scale : {0.7, 1.0, 2.0})
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = Definition;
		Desc.WorldTransform = FTransform(FRotator(0, 37, 0), FVector(Entities.Num() * 400, 0, 20 * Scale), FVector(Scale));
		Entities.Add(Objects->CreateEntity(Desc));
	}
	FWorldObjectCollisionSource Source;
	Source.ViewDirection = FVector::ForwardVector;
	Source.PawnContactBounds = FBox::BuildAABB(FVector(-500, 0, 100), FVector(10));
	Source.ImmediateBounds = FBox::BuildAABB(FVector(400, 0, 50), FVector(1000));
	Source.PrefetchBounds = Source.RetentionBounds = Source.ImmediateBounds;
	Source.Revision = 1;
	Collision->RegisterSource(Source);
	Collision->FlushImmediateCollisionChanges();
	for (const auto Entity : Entities)
	{
		const auto Transform = Objects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity)->WorldTransform;
		// 可见侧边内 1cm 处，旧 75% 碰撞盒完全漏掉这条射线。
		const FVector Top = Transform.TransformPosition(FVector(0, VisibleBounds.Max.Y - 1, VisibleBounds.Max.Z));
		FHitResult Hit;
		TestTrue(TEXT("Dormant 边缘与可见模型同位置阻挡 Pawn"),
			World->LineTraceSingleByChannel(Hit, Top + FVector(0, 0, 50), Top - FVector(0, 0, 100), ECC_Pawn)
			&& Hit.GetActor() != Floor && Hit.ImpactPoint.Equals(Top, 0.1));
		TestTrue(TEXT("同一落地木块可唤醒"), Objects->ActivatePhysics(Entity, FVector::ZeroVector));
		const auto* Proxy = Objects->GetProxy(Entity);
		auto* Actor = Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
		if (!TestNotNull(TEXT("唤醒建立物理投影"), Actor)) return false;
		TestTrue(TEXT("Awake 碰撞盒与实际 Mesh Bounds 一致"),
			Actor->GetPhysicsBox()->GetUnscaledBoxExtent().Equals(VisibleBounds.GetExtent(), 0.01));
		Actor->ReleasePhysicsImmediately();
	}
	World->BeginPlay();
	for (int32 Frame = 0; Frame < 360; ++Frame)
	{
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	}
	for (const auto Entity : Entities)
	{
		const auto* Transform = Objects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity);
		const auto* Motion = Objects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity);
		TestTrue(TEXT("真实 Chaos 休眠回收 Proxy"), Motion && Motion->State == EWorldObjectMotionState::Dormant && !Objects->GetProxy(Entity));
		const double BottomZ = Transform ? VisibleBounds.TransformBy(Transform->WorldTransform).Min.Z : -1000;
		TestTrue(FString::Printf(TEXT("唤醒再休眠后可见底面不陷入地面：%.3fcm"), BottomZ), BottomZ >= -0.5);
	}
	return true;
}

#endif
