#if WITH_DEV_AUTOMATION_TESTS

#include "Definition/WorldObjectDefinition.h"
#include "ElementSandboxWorldObjects.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Components/SphereComponent.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldObjectScaleBenchmark,
	"ElementSandbox.WorldObjects.Benchmark.Static10kPortable1kActive100",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FWorldObjectScaleBenchmark::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("WorldObjectScaleBenchmark"),
		nullptr,
		true);
	check(World);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UWorldObjectWorldSubsystem* Subsystem =
		World->GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!TestNotNull(TEXT("Benchmark WorldObject Subsystem"), Subsystem))
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}

	UWorldObjectDefinition* StaticDefinition = NewObject<UWorldObjectDefinition>(World);
	StaticDefinition->DefinitionId = TEXT("BenchmarkPermanentStatic");
	StaticDefinition->SpatialClass = EWorldObjectSpatialClass::PermanentStatic;
	StaticDefinition->InteractionLocalBounds = FBox(FVector(-10.0), FVector(10.0));
	UWorldObjectDefinition* PortableDefinition = NewObject<UWorldObjectDefinition>(World);
	PortableDefinition->DefinitionId = TEXT("BenchmarkPortable");
	PortableDefinition->SpatialClass = EWorldObjectSpatialClass::Portable;
	PortableDefinition->InteractionLocalBounds = FBox(FVector(-5.0), FVector(5.0));

	const double CreateStart = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < 10000; ++Index)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = StaticDefinition;
		Desc.WorldTransform.SetLocation(FVector(
			(Index % 100) * 40.0,
			(Index / 100) * 40.0,
			0.0));
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		if (!Subsystem->CreateEntity(Desc).IsSet())
		{
			AddError(TEXT("10k PermanentStatic 创建失败"));
			break;
		}
	}
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		FWorldObjectCreateDesc Desc;
		Desc.Definition = PortableDefinition;
		Desc.WorldTransform.SetLocation(FVector(
			(Index % 50) * 35.0,
			4500.0 + (Index / 50) * 35.0,
			0.0));
		Desc.MotionState = EWorldObjectMotionState::Dormant;
		if (!Subsystem->CreateEntity(Desc).IsSet())
		{
			AddError(TEXT("1k Dormant Portable 创建失败"));
			break;
		}
	}

	TArray<TObjectPtr<AActor>> ActiveActors;
	ActiveActors.Reserve(100);
	for (int32 Index = 0; Index < 100; ++Index)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		USphereComponent* Root = NewObject<USphereComponent>(Actor);
			Actor->AddInstanceComponent(Root);
			Actor->SetRootComponent(Root);
			Root->SetCastShadow(false);
			Root->RegisterComponent();
		UWorldObjectProxyComponent* Proxy = NewObject<UWorldObjectProxyComponent>(Actor);
		Actor->AddInstanceComponent(Proxy);
		Proxy->RegisterComponent();
		Actor->SetActorLocation(FVector(Index * 30.0, 5200.0, 0.0));

		FWorldObjectCreateDesc Desc;
		Desc.Definition = PortableDefinition;
		Desc.WorldTransform = Actor->GetActorTransform();
		Desc.MotionState = EWorldObjectMotionState::Attached;
		Desc.Proxy = Proxy;
		if (!Subsystem->CreateEntity(Desc).IsSet())
		{
			AddError(TEXT("100 Active 创建失败"));
			Actor->Destroy();
			break;
		}
		ActiveActors.Add(Actor);
	}
	const double CreateMilliseconds =
		(FPlatformTime::Seconds() - CreateStart) * 1000.0;
	const double StaticBuildStart = FPlatformTime::Seconds();
	Subsystem->GetSpatialIndex().RebuildStaticIfDirty();
	const double StaticBuildMilliseconds =
		(FPlatformTime::Seconds() - StaticBuildStart) * 1000.0;
	for (AActor* ActiveActor : ActiveActors)
	{
		ActiveActor->AddActorWorldOffset(FVector(100.0, 0.0, 0.0));
	}
	const double ActiveSyncStart = FPlatformTime::Seconds();
	FWorldDelegates::OnWorldPostActorTick.Broadcast(World, LEVELTICK_All, 1.0f / 60.0f);
	const double ActiveSyncMilliseconds =
		(FPlatformTime::Seconds() - ActiveSyncStart) * 1000.0;

	FWorldObjectSpatialQueryScratch Scratch;
	TArray<FWorldObjectEntityHandle> Hits;
	const double QueryStart = FPlatformTime::Seconds();
	Subsystem->GetSpatialIndex().QueryOverlaps(
		FBox(FVector(0.0, 0.0, -20.0), FVector(4000.0, 4000.0, 20.0)),
		Scratch,
		Hits);
	const double QueryMilliseconds =
		(FPlatformTime::Seconds() - QueryStart) * 1000.0;
	const FWorldObjectRuntimeStats Stats = Subsystem->GetRuntimeStats();

	TestEqual(TEXT("Benchmark PermanentStatic 数量"), Stats.PermanentStaticCount, 10000);
	TestEqual(TEXT("Benchmark Portable 数量"), Stats.PortableCount, 1100);
	TestEqual(TEXT("Benchmark Active 数量"), Stats.ActiveCount, 100);
	TestEqual(TEXT("一帧只访问 100 个 Active"), Stats.LastSampledActiveCount, 100);
	TestEqual(TEXT("100 个 Active 越过 Fat Bounds 后重插"),
		Stats.DynamicReinsertCount,
		int64(100));
	UE_LOG(
		LogElementSandboxWorldObjects,
		Display,
		TEXT("WorldObject benchmark: create=%.3fms static-build=%.3fms active-sync=%.3fms query=%.3fms active-visits=%d reinserts=%lld query-nodes=%d hits=%d registry-bytes=%lld spatial-bytes=%lld total-bytes=%lld"),
		CreateMilliseconds,
		StaticBuildMilliseconds,
		ActiveSyncMilliseconds,
		QueryMilliseconds,
		Stats.LastSampledActiveCount,
		Stats.DynamicReinsertCount,
		Scratch.GetLastVisitedNodeCount(),
		Hits.Num(),
		Stats.RegistryAllocatedBytes,
		Stats.SpatialAllocatedBytes,
		Stats.EstimatedAllocatedBytes);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}


#endif
