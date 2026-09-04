#if !UE_BUILD_SHIPPING
#include "MeteorClientWorldSubsystem.h"
#include "MeteorBallisticKernel.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjectCreateDesc.h"
#include "WorldStorageSubsystem.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderTimer.h"
#include "DynamicRHI.h"
#include "UObject/UObjectIterator.h"

using namespace UE::ElementSandbox::Meteor;
namespace
{
	/** 显式命令行启用的离线表现基准，使用固定 Kernel 输入；不触碰用户存档或在线 Authority。 */
	class FMeteorVisualBenchmark final
	{
	public:
		FMeteorVisualBenchmark()
		{
			if (!FParse::Value(FCommandLine::Get(), TEXT("MeteorVisualBenchmark="), Count) || Count <= 0) return;
			if (!FParse::Value(FCommandLine::Get(), TEXT("MeteorVisualBenchmarkOutput="), Output)) return;
				FParse::Value(FCommandLine::Get(), TEXT("MeteorVisualBenchmarkHold="), HoldSeconds);
				FParse::Value(FCommandLine::Get(), TEXT("MeteorVisualBenchmarkLanesPerPage="), LanesPerPage);
				LanesPerPage = FMath::Clamp(LanesPerPage, 1, WorkPageCapacity);
			Handle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FMeteorVisualBenchmark::Tick);
		}
		~FMeteorVisualBenchmark() { FWorldDelegates::OnWorldPostActorTick.Remove(Handle); }
	private:
		void Prepare(UWorld& World)
		{
			const double Started = FPlatformTime::Seconds();
			auto* Storage = World.GetSubsystem<UWorldStorageSubsystem>();
			auto* Client = World.GetSubsystem<UMeteorClientWorldSubsystem>();
			const auto Config = Client->GetRuntimeConfig();
			FRandomStream Random(730194);
			const int32 Side = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count)));
			FMeteorWorkPage Work;
			auto Flush = [&]()
			{
				if (Work.Num() == 0) return;
				auto Page = MakeShared<FMeteorTrajectoryPage>();
				check(FMeteorBallisticKernel::CompilePage(Work, Config, {70001}, Pages.Num() + 1, *Page));
				MaxFlightSeconds = FMath::Max(MaxFlightSeconds, Page->ValidUntilSeconds - Page->ValidFromSeconds);
				Pages.Add(Page); Client->QueuePreparedTrajectoryPage(Page);
			};
			for (int32 Ordinal = 0; Ordinal < Count; ++Ordinal)
			{
					if (Ordinal % LanesPerPage == 0) { Flush(); Work.Reset({0,1}, TEXT("WorldObject.WoodBlock"), FVector3d::ZeroVector); }
				FMeteorDebrisSeed Seed;
				Seed.Key = {{70001}, static_cast<uint32>(Ordinal)};
				const auto Id = Storage->AllocateEntityId(); Ids.Add(Id);
				Seed.WorldEntityId = Id;
				Seed.RenderArchetypeId = TEXT("WorldObject.WoodBlock");
				Seed.StartPosition = FVector((Ordinal % Side - Side * 0.5) * 100, (Ordinal / Side - Side * 0.5) * 100, 300 + Random.FRand() * 1500);
				Seed.StartRotation = FRotator(Random.FRand() * 360, Random.FRand() * 360, Random.FRand() * 360).Quaternion();
				const FVector Radial = FVector(Seed.StartPosition.X, Seed.StartPosition.Y, 0).GetSafeNormal();
				const double SpeedScale = Count <= 256 ? 0.15 : 1.0;
				Seed.InitialVelocity = Radial * ((1500 + Random.FRand() * 2000) * SpeedScale) + FVector(0, 0, (6500 + Random.FRand() * 2500) * SpeedScale);
				Seed.AngularVelocityDegrees = FVector(Random.FRand() * 150, Random.FRand() * 150, Random.FRand() * 150);
				Seed.Scale = FVector3f::OneVector; Seed.ProductLocalBounds = FBox3f(FVector3f(-70,-24,-20), FVector3f(70,24,20));
				Seed.VisualRadius = 80; Seed.StartTimeSeconds = Seed.ValidFromSeconds = ActivationTime;
				Seed.LatestComputeStartSeconds = World.GetTimeSeconds();
				check(Work.Append(Seed));
			}
			Flush();
			UE_LOG(LogTemp, Display, TEXT("MeteorVisualBenchmark: Prepared %d deterministic lanes in %.3f ms; max flight %.3fs."), Count, (FPlatformTime::Seconds()-Started)*1000, MaxFlightSeconds);
		}
		void Activate(UWorld& World)
		{
			for (const auto& Page : Pages)
			{
				FMeteorTrajectoryActivation Command;
				Command.BurstId = Page->BurstId; Command.PageId = Page->PageId; Command.Revision = Page->Revision;
				Command.SourceWorldEntityId = FWorldEntityId(900000000); Command.SourceTombstoneRevision = 2;
				Command.AuthorityStartTimeSeconds = ActivationTime; Command.Ordinals = Page->Ordinals;
				World.GetSubsystem<UMeteorClientWorldSubsystem>()->QueueTrajectoryActivation(Command);
			}
		}
		void Settle(UWorld& World)
		{
			auto* Objects = World.GetSubsystem<UWorldObjectWorldSubsystem>();
			const int32 End = FMath::Min(Count, Settled + 512);
			TArray<FWorldObjectCreateDesc> Descs;
			for (int32 I = Settled; I < End; ++I)
			{
					const auto& Page = *Pages[I / LanesPerPage]; const int32 Lane = I % LanesPerPage;
				auto& Desc = Descs.AddDefaulted_GetRef();
				Desc.Definition = GetMutableDefault<UWoodBlockWorldObjectDefinition>();
				Desc.ReservedWorldEntityId = Ids[I];
				Desc.WorldTransform = Page.GetRestTransform(Lane);
				Desc.MotionState = EWorldObjectMotionState::Dormant;
			}
			FWorldObjectStagedCreateBatch Batch; TArray<FWorldObjectEntityHandle> Entities;
			if (!Objects->StageCreateEntities(Descs, Batch) || !Objects->CommitStagedCreateEntities(Batch, Entities))
			{
				UE_LOG(LogTemp, Error, TEXT("MeteorVisualBenchmark: Settlement failed at %d."), Settled); Finish(); return;
			}
			TArray<FMeteorSettlementMapping> Mappings;
			for (int32 I = 0; I < Entities.Num(); ++I)
			{
				auto& Mapping = Mappings.AddDefaulted_GetRef(); Mapping.Debris = {{70001}, static_cast<uint32>(Settled + I)};
				Mapping.WorldEntityId = Objects->GetWorldEntityId(Entities[I]);
			}
			World.GetSubsystem<UMeteorClientWorldSubsystem>()->QueueSettlementMappings(Mappings);
			Settled = End;
			if (Settled == Count) { StaticStart = World.GetTimeSeconds() + 8; UE_LOG(LogTemp, Display, TEXT("MeteorVisualBenchmark: All %d ordinary objects committed."), Count); }
		}
		void Tick(UWorld* World, ELevelTick, float)
		{
			if (Done || !World || !World->IsGameWorld() || World->GetNetMode() != NM_Standalone) return;
			const double Now = World->GetTimeSeconds();
			if (Now < 12 || !World->GetFirstPlayerController()) return;
			if (!bStarted)
			{
				bStarted = true; BeginTime = Now; ActivationTime = Now + 15;
				// 离线基准没有网络握手，移除常驻加载帘；真实多人验收仍使用正式 UI。
				GEngine->GameViewport->RemoveAllViewportWidgets();
				World->GetFirstPlayerController()->ConsoleCommand(TEXT("r.SetRes 1280x720w"));
				auto* Floor = World->SpawnActor<AStaticMeshActor>();
				Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
				// 近景检查避开 DefaultMap 地面的共面深度竞争；性能对照保持同一固定场景。
				Floor->SetActorLocation(FVector(0,0,Count <= 256 ? -55 : -50)); Floor->SetActorScale3D(FVector(2000,2000,1));
				Floor->GetStaticMeshComponent()->SetCastShadow(false);
				if (FParse::Param(FCommandLine::Get(), TEXT("MeteorVisualInspection"))) World->GetWorldSettings()->SetTimeDilation(0.2f);
				Csv = TEXT("phase,elapsed_s,frame_ms,gt_ms,rt_ms,gpu_ms,process_mb,native_instances,visible_instances,collision_instances,settled\n");
				auto* Camera = World->SpawnActor<ACameraActor>();
				const FVector Position = Count <= 256 ? FVector(1800,-2800,1700) : FVector(55000,-80000,55000);
				const FVector Target = Count <= 256 ? FVector(0,0,400) : FVector(0,0,16000);
				Camera->SetActorLocationAndRotation(Position, (Target - Position).Rotation()); Camera->GetCameraComponent()->SetFieldOfView(65);
				World->GetFirstPlayerController()->SetViewTarget(Camera);
				LastWall = FPlatformTime::Seconds(); Prepare(*World); return;
			}
			if (!bActivated && Now >= ActivationTime) { bActivated = true; Activate(*World); }
			if (bActivated && Now >= ActivationTime + MaxFlightSeconds + 0.25 && Settled < Count) Settle(*World);
			const TCHAR* Phase = !bActivated ? TEXT("create") : Now < ActivationTime + MaxFlightSeconds ? TEXT("flight") : (StaticStart < 0 || Now < StaticStart) ? TEXT("settle") : TEXT("static");
			if (Now >= NextSample)
			{
				MemoryMB = FPlatformMemory::GetStats().UsedPhysical / (1024.0*1024.0);
				NativeInstances = VisibleInstances = CollisionInstances = 0;
				for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
					if (It->GetWorld() == World && It->IsRegistered())
					{
						NativeInstances += It->GetInstanceCount();
						if (It->IsVisible() && !It->bHiddenInGame) VisibleInstances += It->GetInstanceCount();
						if (It->GetCollisionEnabled() != ECollisionEnabled::NoCollision) CollisionInstances += It->GetInstanceCount();
					}
				NextSample = Now + 1;
			}
			const double Wall = FPlatformTime::Seconds();
				Csv += FString::Printf(TEXT("%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%d,%d,%d,%d\n"), Phase, Now-BeginTime, (Wall-LastWall)*1000,
					FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime),
					FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()), MemoryMB, NativeInstances, VisibleInstances, CollisionInstances, Settled);
			LastWall = Wall;
			if (StaticStart > 0 && Now >= StaticStart + HoldSeconds) Finish();
		}
		void Finish()
		{
			Done = true; IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
			FFileHelper::SaveStringToFile(Csv, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogTemp, Display, TEXT("MeteorVisualBenchmark: Finished lanes=%d settled=%d native=%d csv=%s"), Count, Settled, NativeInstances, *Output);
			FPlatformMisc::RequestExit(false);
		}
			FDelegateHandle Handle;
			int32 LanesPerPage = WorkPageCapacity;
			int32 Count = 0, Settled = 0, NativeInstances = 0, VisibleInstances = 0, CollisionInstances = 0;
		bool Done = false, bStarted = false, bActivated = false;
		double BeginTime = 0, ActivationTime = 0, MaxFlightSeconds = 0, StaticStart = -1, HoldSeconds = 12, LastWall = 0, NextSample = 0, MemoryMB = 0;
		FString Output, Csv;
		TArray<TSharedPtr<FMeteorTrajectoryPage>> Pages;
		TArray<FWorldEntityId> Ids;
	};
	FMeteorVisualBenchmark GVisualBenchmark;
}
#endif
