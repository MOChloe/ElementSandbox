#if WITH_DEV_AUTOMATION_TESTS
#include "MeteorWorldSubsystem.h"
#include "MeteorClientWorldSubsystem.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "Tree/SettlementTreeDefinition.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjectCreateDesc.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

using namespace UE::ElementSandbox::Meteor;
namespace
{
	/** 独立进程网络回归：仅显式参数启用，要求调用脚本提供隔离存档和空种子。 */
	class FMeteorNetworkProbe final
	{
	public:
		FMeteorNetworkProbe()
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("MeteorNetworkProbe")))
				TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FMeteorNetworkProbe::Tick);
		}
		~FMeteorNetworkProbe() { FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle); }
	private:
		struct FObservedInstance
		{
			TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component;
				FTransform Rest;
				FTransform InitialTransform;
				int32 Index = INDEX_NONE;
				bool bReportedMovement = false;
			EMeteorClientDebrisState State = EMeteorClientDebrisState::Missing;
		};
		void Initialize(UWorld& World)
		{
			ObservedWorld = &World;
			if (World.GetNetMode() == NM_Client)
			{
				World.GetSubsystem<UMeteorClientWorldSubsystem>()->OnPresentationChanges().AddLambda(
					[this](TConstArrayView<FMeteorClientPresentationLane> Changes)
						{
							LastChangeTime = ObservedWorld->GetTimeSeconds();
							for (const auto& Change : Changes)
						{
							auto& Item = Instances.FindOrAdd(Change.WorldEntityId);
							Item.State = Change.State;
							if (!Change.RenderArchetypeId.IsNone()) Item.Rest = Change.RestTransform;
							Prepared += Change.State == EMeteorClientDebrisState::Prepared;
							Activated += Change.State == EMeteorClientDebrisState::Flying;
						}
					});
			}
			else
			{
				auto* Meteor = World.GetSubsystem<UMeteorWorldSubsystem>();
				Meteor->OnTrajectoryPagePrepared().AddLambda([this](const FMeteorTrajectoryPage& Page)
				{
					auto* Objects = ObservedWorld->GetSubsystem<UWorldObjectWorldSubsystem>();
					for (int32 Lane = 0; Lane < Page.Num(); ++Lane)
					{
						const auto Id = Page.WorldEntityIds[Lane];
						bValid &= !Reserved.Contains(Id) && !Objects->FindEntity(Id).IsSet();
						Reserved.Add(Id);
					}
					UE_LOG(LogTemp, Display, TEXT("MeteorNetworkProbe: PREPARED lanes=%d version=%u"), Page.Num(), TrajectoryPayloadFormatVersion);
				});
				Meteor->OnSettlementPublished().AddLambda([this](TConstArrayView<FMeteorSettlementMapping> Mappings)
				{
					for (const auto& Mapping : Mappings)
					{
						bValid &= Reserved.Contains(Mapping.WorldEntityId) && !Settled.Contains(Mapping.WorldEntityId);
						Settled.Add(Mapping.WorldEntityId);
					}
				});
			}
		}
		void TickServer(UWorld& World)
		{
			const double Now = World.GetTimeSeconds();
			int32 Players = 0;
			for (auto It = World.GetPlayerControllerIterator(); It; ++It)
				if (It->Get() && It->Get()->GetPawn()) ++Players;
			MaxPlayers = FMath::Max(MaxPlayers, Players);
			auto* Meteor = World.GetSubsystem<UMeteorWorldSubsystem>();
			if (StartTime < 0 && Players && Now > 10)
			{
				const FVector Pawn = World.GetFirstPlayerController()->GetPawn()->GetActorLocation();
				Center = FVector(Pawn.X + 5000, Pawn.Y, 0);
				auto* Objects = World.GetSubsystem<UWorldObjectWorldSubsystem>();
				for (int32 I = 0; I < 16; ++I)
				{
					FWorldObjectCreateDesc Desc;
					Desc.Definition = Objects->FindDefinition(GetDefault<USettlementTreeDefinition>()->DefinitionId);
					Desc.WorldTransform = FTransform(Center + FVector((I % 4 - 1.5) * 400, (I / 4 - 1.5) * 400, 0));
					Desc.MotionState = EWorldObjectMotionState::Dormant;
					bValid &= Objects->CreateEntity(Desc).IsSet();
				}
				if (!bValid) { UE_LOG(LogTemp, Error, TEXT("MeteorNetworkProbe: FAIL source creation")); bDone = true; return; }
				auto Config = Meteor->GetRuntimeConfig();
				Config.ShockwaveRadius = 2000; Config.ImpactCoreRadius = 1500; Config.ShockwaveSpeed = 2500;
				Config.DebrisSpeedRange = FVector2f(6000, 7000);
				Config.DebrisLowElevationDegrees = FVector2f(60, 65);
				Config.DebrisMediumElevationDegrees = FVector2f(65, 70);
				Config.DebrisHighElevationDegrees = FVector2f(70, 75);
				Config.DebrisGroundScatterFraction = 0;
				Config.GravityZ = -980;
				Meteor->OverrideRuntimeConfigForTesting(Config);
				StartTime = Now + 5;
			}
			if (StartTime >= 0 && !bScheduled && Now >= StartTime)
			{
				FMeteorBurstId Burst;
				bScheduled = Meteor->ScheduleStrike(Center, Now + 6, Burst);
				bValid &= bScheduled;
				UE_LOG(LogTemp, Display, TEXT("MeteorNetworkProbe: SCHEDULED burst=%llu"), Burst.Value);
			}
			if (bScheduled && !Meteor->HasActiveBurst() && !Settled.IsEmpty() && MaxPlayers >= 2)
			{
				bValid &= Reserved.Num() == Settled.Num();
				UE_LOG(LogTemp, Display, TEXT("MeteorNetworkProbe: %s server players=%d reserved=%d settled=%d"),
					bValid ? TEXT("PASS") : TEXT("FAIL"), MaxPlayers, Reserved.Num(), Settled.Num());
				bDone = true;
			}
		}
		void TickClient(UWorld& World)
		{
			auto* Wood = World.GetSubsystem<UWoodProductPresentationWorldSubsystem>();
			auto* Objects = World.GetSubsystem<UWorldObjectWorldSubsystem>();
			bool bComplete = !Instances.IsEmpty();
			int32 Continuity = 0;
			for (auto& Pair : Instances)
			{
				auto& Item = Pair.Value;
				UHierarchicalInstancedStaticMeshComponent* Component = nullptr;
				int32 Index = INDEX_NONE;
				if (!Wood->FindInstance(Pair.Key, Component, Index)) { bComplete = false; continue; }
					if (Item.Index == INDEX_NONE)
					{
						Item.Component = Component; Item.Index = Index;
						Component->GetInstanceTransform(Index, Item.InitialTransform, true);
				}
				else if (Item.Component.Get() != Component || Item.Index != Index) bValid = false;
				const auto Entity = Objects->FindEntity(Pair.Key);
				const auto* Transform = Objects->GetRegistry().FindFragment<FWorldObjectTransformFragment>(Entity);
					FTransform Render; Component->GetInstanceTransform(Index, Render, true);
					if (!Item.bReportedMovement && !Render.Equals(Item.InitialTransform, 0.01))
					{
						Item.bReportedMovement = true;
						if (++MovedInstances <= 3)
							UE_LOG(LogTemp, Display, TEXT("MeteorNetworkProbe: MOVED id=%llu initial=%s render=%s trajectory=%s ecs=%s"),
								Pair.Key.GetValue(), *Item.InitialTransform.ToString(), *Render.ToString(), *Item.Rest.ToString(),
								Transform ? *Transform->WorldTransform.ToString() : TEXT("missing"));
					}
				if (Item.State != EMeteorClientDebrisState::Settled || !Transform || Component->NumCustomDataFloats != 0)
					bComplete = false;
				else
				{
					bValid &= Render.Equals(Transform->WorldTransform, 0.1);
					++Continuity;
				}
			}
				// 页和 Settlement 分批到达；不能把第一个已落地物品误当成整场爆炸完成。
				if (bComplete && World.GetTimeSeconds() > 60 && World.GetTimeSeconds() - LastChangeTime > 3)
			{
				bValid &= Prepared > 0 && Activated > 0 && Wood->GetInstanceRemoveCount() == 0
					&& Wood->GetTransformUpdateCount() == 0 && Wood->GetInstanceCount() == Instances.Num();
				UE_LOG(LogTemp, Display, TEXT("MeteorNetworkProbe: %s client prepared=%d activated=%d settled=%d instances=%d adds=%llu removes=%llu transform_updates=%llu"),
					bValid ? TEXT("PASS") : TEXT("FAIL"), Prepared, Activated, Continuity, Wood->GetInstanceCount(),
					Wood->GetInstanceAddCount(), Wood->GetInstanceRemoveCount(), Wood->GetTransformUpdateCount());
				bDone = true;
			}
		}
		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld()
				|| (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_DedicatedServer)) return;
			if (!ObservedWorld.IsValid()) Initialize(*World);
			if (ObservedWorld != World) return;
			if (World->GetNetMode() == NM_Client) TickClient(*World); else TickServer(*World);
			if (!bDone && World->GetTimeSeconds() > 100)
			{
				UE_LOG(LogTemp, Error, TEXT("MeteorNetworkProbe: FAIL timeout reserved=%d settled=%d prepared=%d activated=%d observed=%d"),
					Reserved.Num(), Settled.Num(), Prepared, Activated, Instances.Num());
				bDone = true;
			}
		}
		FDelegateHandle TickHandle;
		TWeakObjectPtr<UWorld> ObservedWorld;
		TSet<FWorldEntityId> Reserved, Settled;
		TMap<FWorldEntityId, FObservedInstance> Instances;
		FVector Center;
			double StartTime = -1;
			double LastChangeTime = 0;
			int32 Prepared = 0, Activated = 0, MaxPlayers = 0, MovedInstances = 0;
		bool bScheduled = false, bValid = true, bDone = false;
	};
	FMeteorNetworkProbe GNetworkProbe;
}
#endif
