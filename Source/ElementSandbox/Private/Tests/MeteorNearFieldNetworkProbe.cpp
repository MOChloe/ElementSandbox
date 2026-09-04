#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ElementSandboxCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Game/ElementSandboxPlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "MeteorClientWorldSubsystem.h"
#include "MeteorWorldSubsystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "Tree/SettlementTreeDefinition.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"

namespace
{
	using namespace UE::ElementSandbox::Meteor;

	/** 显式空存档双客户端回归：真实预演、Activate、落地和网络同时推进时检查近场胶囊。 */
	class FMeteorNearFieldNetworkProbe final
	{
	public:
		FMeteorNearFieldNetworkProbe()
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("MeteorNearFieldNetworkProbe")))
				TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FMeteorNearFieldNetworkProbe::Tick);
		}
		~FMeteorNearFieldNetworkProbe()
		{
			FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle);
			if (ServerMeteor.IsValid())
			{
				ServerMeteor->OnTrajectoryPagePrepared().Remove(PageHandle);
				ServerMeteor->OnTrajectoryActivated().Remove(ActivationHandle);
				ServerMeteor->OnSettlementPublished().Remove(SettlementHandle);
			}
			if (ClientMeteor.IsValid()) ClientMeteor->OnPresentationChanges().Remove(PresentationHandle);
		}

	private:
		struct FNearLane
		{
			FWorldEntityId Id;
			FTransform Rest;
			double Duration = 0.0, Due = -1.0, FirstVisible = -1.0;
			bool bChecked = false, bSettled = false;
		};

		void Fail(const TCHAR* Reason)
		{
			UE_LOG(LogTemp, Error, TEXT("MeteorNearFieldProbe: FAIL %s"), Reason);
			bDone = true;
		}

		void HandlePage(const FMeteorTrajectoryPage& Page)
		{
			for (int32 Index = 0; Index < Page.Num(); ++Index)
			{
				const FTransform Rest = Page.GetRestTransform(Index);
				if (FVector::DistSquared2D(Rest.GetLocation(), Origin) > FMath::Square(400.0)) continue;
				FNearLane& Lane = NearLanes.FindOrAdd(Page.Ordinals[Index]);
				Lane.Id = Page.WorldEntityIds[Index];
				Lane.Rest = Rest;
				Lane.Duration = Page.ImpactDurations[Index] + Page.SettlingDurations[Index];
			}
		}

		void HandleActivation(const FMeteorTrajectoryActivation& Activation)
		{
			for (const uint32 Ordinal : Activation.Ordinals)
				if (FNearLane* Lane = NearLanes.Find(Ordinal)) Lane->Due = Activation.AuthorityStartTimeSeconds + Lane->Duration;
		}

		void HandleSettlement(TConstArrayView<FMeteorSettlementMapping> Mappings)
		{
			if (LastSettlementFrame != GFrameCounter) { LastSettlementFrame = GFrameCounter; FrameSettlements = 0; }
			FrameSettlements += Mappings.Num();
			MaxFrameSettlements = FMath::Max(MaxFrameSettlements, FrameSettlements);
			for (const auto& Mapping : Mappings)
			{
				FNearLane* Lane = NearLanes.Find(Mapping.Debris.DebrisOrdinal);
				if (!Lane || Lane->Due < 0.0) continue;
				Lane->bSettled = true;
				MaxSettlementDelay = FMath::Max(MaxSettlementDelay, ObservedWorld->GetTimeSeconds() - Lane->Due);
				UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: SETTLED id=%llu delay=%.3f backlog=%d"),
					Lane->Id.GetValue(), ObservedWorld->GetTimeSeconds() - Lane->Due, ServerMeteor->GetAuthorityStats().SettlementBacklog);
			}
		}

		void HandlePresentation(TConstArrayView<FMeteorClientPresentationLane> Changes)
		{
			for (const auto& Change : Changes)
			{
				if (Change.State == EMeteorClientDebrisState::Prepared || Change.State == EMeteorClientDebrisState::Canceled
					|| Change.RenderArchetypeId.IsNone()
					|| FVector::DistSquared2D(Change.RestTransform.GetLocation(), Origin) > FMath::Square(400.0)) continue;
				FNearLane& Lane = NearLanes.FindOrAdd(Change.Ordinal);
				Lane.Id = Change.WorldEntityId;
				Lane.Rest = Change.RestTransform;
				Lane.Due = Change.State == EMeteorClientDebrisState::Settled ? ObservedWorld->GetTimeSeconds()
					: Change.LocalStartTimeSeconds + Change.ImpactDurationSeconds + Change.SettlingDurationSeconds;
			}
		}

		void TickServer(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			TArray<AElementSandboxPlayerController*> Players;
			for (auto It = World.GetPlayerControllerIterator(); It; ++It)
			{
				auto* Player = Cast<AElementSandboxPlayerController>(It->Get());
				if (Player && Player->GetPawn() && Player->PlayerState) Players.Add(Player);
			}
			if (Players.Num() != 2) return;
			Players.Sort([](const auto& A, const auto& B) { return A.PlayerState->GetPlayerId() < B.PlayerState->GetPlayerId(); });
			if (!bOriginSet) { Origin = Players[0]->GetPawn()->GetActorLocation(); bOriginSet = true; }
			constexpr int32 NearbySources = 256, SourceCount = NearbySources + 32768;
			if (CreatedSources < SourceCount)
			{
				TArray<FWorldObjectCreateDesc> Descs;
				for (int32 Offset = 0; Offset < 512 && CreatedSources + Offset < SourceCount; ++Offset)
				{
					const int32 Index = CreatedSources + Offset;
					const int32 FarIndex = Index - NearbySources;
					const FVector Position = Index < NearbySources
						? FVector(Origin.X + (Index % 16 - 7.5) * 350.0, Origin.Y + (Index / 16 - 7.5) * 350.0, 0)
						: FVector(Origin.X + 50000 + (FarIndex % 128) * 600.0, Origin.Y - 76800 + (FarIndex / 128) * 600.0, 0);
					FWorldObjectCreateDesc& Desc = Descs.AddDefaulted_GetRef();
					Desc.Definition = Objects.FindDefinition(GetDefault<USettlementTreeDefinition>()->DefinitionId);
					Desc.WorldTransform.SetLocation(Position);
					SourceChunks.Add(FWorldChunkCoord::FromWorldLocation(Position));
				}
				FWorldObjectStagedCreateBatch Staged;
				TArray<FWorldObjectEntityHandle> Created;
				if (!Objects.StageCreateEntities(Descs, Staged) || !Objects.CommitStagedCreateEntities(Staged, Created))
				{
					Objects.RollbackStagedCreateEntities(Staged); Fail(TEXT("fixture creation")); return;
				}
				CreatedSources += Created.Num();
				return;
			}
			if (!ServerMeteor.IsValid())
			{
				for (const auto* Player : Players)
				{
					const auto* Streaming = Player->FindComponentByClass<UWorldChunkStreamingComponent>();
					if (!Streaming || !Streaming->GetStreamingStats().bActivationCoreReady
						|| Streaming->GetStreamingStats().PendingLiveDeltaCount != 0) return;
					for (const auto Coord : SourceChunks) if (!Streaming->IsAuthorityChunkReadyForLiveMutation(Coord)) return;
				}
				ServerMeteor = World.GetSubsystem<UMeteorWorldSubsystem>();
				PageHandle = ServerMeteor->OnTrajectoryPagePrepared().AddRaw(this, &FMeteorNearFieldNetworkProbe::HandlePage);
				ActivationHandle = ServerMeteor->OnTrajectoryActivated().AddRaw(this, &FMeteorNearFieldNetworkProbe::HandleActivation);
				SettlementHandle = ServerMeteor->OnSettlementPublished().AddRaw(this, &FMeteorNearFieldNetworkProbe::HandleSettlement);
				FMeteorBurstId Burst;
				StartTime = World.GetTimeSeconds();
				if (!ServerMeteor->ScheduleStrike(FVector(Origin.X + 20000, Origin.Y, 0), StartTime + 3.0, Burst))
				{ Fail(TEXT("real meteor scheduling")); return; }
				UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: START sources=%d"), CreatedSources);
			}
			if (World.GetTimeSeconds() - StartTime < 45.0) return;
			int32 Settled = 0;
			for (const auto& Pair : NearLanes)
			{
				if (Pair.Value.Due < 0.0) continue;
				Settled += Pair.Value.bSettled;
				if (!Pair.Value.bSettled && World.GetTimeSeconds() - Pair.Value.Due > 2.0)
				{ Fail(TEXT("near authority settlement starved")); return; }
			}
			if (Settled == 0 || MaxSettlementDelay > 2.0) { Fail(TEXT("near authority settlement deadline")); return; }
			for (const auto* Player : Players)
			{
				if (FVector::Dist2D(Player->GetPawn()->GetActorLocation(), Origin) < 600.0)
				{ Fail(TEXT("bulk traffic starved authority player movement")); return; }
			}
			UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: PASS server nearby=%d max_delay=%.3f max_per_frame=%d total_activated=%u"),
				Settled, MaxSettlementDelay, MaxFrameSettlements, ServerMeteor->GetAuthorityStats().TotalActivatedLaneCount);
			bDone = true;
		}

		void TickClient(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			auto* Controller = Cast<AElementSandboxPlayerController>(World.GetFirstPlayerController());
			auto* Character = Controller ? Cast<AElementSandboxCharacter>(Controller->GetPawn()) : nullptr;
			auto* Wood = World.GetSubsystem<UWoodProductPresentationWorldSubsystem>();
			if (!Character || !Controller->PlayerState || !Wood) return;
			if (!ClientMeteor.IsValid())
			{
				Origin = Character->GetActorLocation();
				ClientMeteor = World.GetSubsystem<UMeteorClientWorldSubsystem>();
				PresentationHandle = ClientMeteor->OnPresentationChanges().AddRaw(this, &FMeteorNearFieldNetworkProbe::HandlePresentation);
			}
			const double Now = World.GetTimeSeconds();
			for (auto& Pair : NearLanes)
			{
				FNearLane& Lane = Pair.Value;
				if (Lane.bChecked || Lane.Due < 0.0 || Now < Lane.Due) continue;
				UHierarchicalInstancedStaticMeshComponent* RenderComponent = nullptr;
				int32 RenderIndex = INDEX_NONE;
				if (!Wood->FindInstance(Lane.Id, RenderComponent, RenderIndex)) continue;
				if (Lane.FirstVisible < 0.0) Lane.FirstVisible = Now;
				const auto Entity = Objects.FindEntity(Lane.Id);
				const auto* Motion = Objects.GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity);
				if (!Motion || Motion->State != EWorldObjectMotionState::Dormant)
				{
					if (Now - Lane.FirstVisible > 2.0)
					{
						UE_LOG(LogTemp, Error, TEXT("MeteorNearFieldProbe: MISSING id=%llu position=%s age=%.3f entity=%d"),
							Lane.Id.GetValue(), *Lane.Rest.GetLocation().ToString(), Now - Lane.FirstVisible, Entity.IsSet());
						Fail(TEXT("visible landed meteor wood has no usable near entity")); return;
					}
					continue;
				}
				const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
				const FVector Rest = Lane.Rest.GetLocation();
				const FVector Center(Rest.X, Rest.Y, Capsule->GetScaledCapsuleHalfHeight() + 2.0);
				FHitResult Hit;
				FCollisionQueryParams Query(SCENE_QUERY_STAT(MeteorNearPawn), false, Character);
				const bool bBlocked = World.SweepSingleByChannel(Hit, Center - FVector(200, 0, 0), Center + FVector(200, 0, 0),
					FQuat::Identity, Capsule->GetCollisionObjectType(), Capsule->GetCollisionShape(), Query,
					FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())) && Hit.bBlockingHit && Hit.Item != INDEX_NONE;
				if (!bBlocked)
				{
					if (Now - Lane.FirstVisible > 2.0) { Fail(TEXT("landed meteor wood does not block client Pawn")); return; }
					continue;
				}
				Lane.bChecked = true;
				++CheckedCount;
				MaxClientDelay = FMath::Max(MaxClientDelay, Now - Lane.FirstVisible);
				if (FirstCheckedTime < 0.0) FirstCheckedTime = Now;
				UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: PAWN_BLOCK id=%llu delay=%.3f hit=%s"),
					Lane.Id.GetValue(), Now - Lane.FirstVisible, *GetNameSafe(Hit.GetComponent()));
			}
			// 测试保留真实 Focus/E 入口；附近目标由现有查询产生，不直接销毁实体模拟成功。
			if (!PickupId.IsSet() && CheckedCount > 0 && Now - FirstCheckedTime >= 15.0)
			{
				auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>();
				for (const auto& Pair : NearLanes)
				{
					if (!Pair.Value.bChecked || !Objects.FindEntity(Pair.Value.Id).IsSet() || !Focus) continue;
					FFocusQueryContext Context;
					FRotator Rotation;
					Controller->GetPlayerViewPoint(Context.ViewOrigin, Rotation);
					Context.ViewDirection = (Pair.Value.Rest.GetLocation() - Context.ViewOrigin).GetSafeNormal();
					Focus->EvaluateFocus(Context);
					const auto* Hit = Focus->GetFocusedHit();
					const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
					FFocusInteractionPrompt Prompt;
					if (Target && Target->WorldEntityId == Pair.Value.Id && Focus->TryResolveFocusedPrompt(Prompt) && Focus->HandleInteract())
					{
						PickupId = Target->WorldEntityId;
						UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: PICKUP_REQUEST id=%llu"), PickupId.GetValue());
						break;
					}
				}
			}
			if (FirstCheckedTime >= 0.0 && Now - FirstCheckedTime >= 20.0)
			{
				const AGameStateBase* State = World.GetGameState();
				int32 FirstPlayer = MAX_int32;
				if (State) for (const APlayerState* Player : State->PlayerArray)
					if (Player) FirstPlayer = FMath::Min(FirstPlayer, Player->GetPlayerId());
				const FVector Direction = Controller->PlayerState->GetPlayerId() == FirstPlayer
					? FVector::ForwardVector : -FVector::RightVector;
				if (bMovementStarted) MaxBackwardCorrection = FMath::Max(MaxBackwardCorrection,
					FVector::DotProduct(LastLocation - Character->GetActorLocation(), Direction));
				LastLocation = Character->GetActorLocation();
				bMovementStarted = true;
				if (Now - FirstCheckedTime < 25.0 && FVector::Dist2D(LastLocation, Origin) < 1200.0)
					Character->AddMovementInput(Direction, 1.0f, true);
			}
			if (FirstCheckedTime < 0.0 || Now - FirstCheckedTime < 35.0) return;
			if (!PickupId.IsSet() || Objects.FindEntity(PickupId).IsSet()) { Fail(TEXT("real meteor pickup did not finish")); return; }
			if (FVector::Dist2D(Character->GetActorLocation(), Origin) < 600.0 || MaxBackwardCorrection > 100.0)
			{ Fail(TEXT("player movement stalled or snapped back during meteor streaming")); return; }
			UE_LOG(LogTemp, Display, TEXT("MeteorNearFieldProbe: PASS client checked=%d max_delay=%.3f activated=%lld moved=%.1f max_backward_cm=%.3f"),
				CheckedCount, MaxClientDelay, ClientMeteor->GetRuntime().GetStats().TotalActivatedLaneCount,
				FVector::Dist2D(Character->GetActorLocation(), Origin), MaxBackwardCorrection);
			bDone = true;
		}

		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld() || (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_DedicatedServer)) return;
			if (!ObservedWorld.IsValid()) ObservedWorld = World;
			if (ObservedWorld != World) return;
			auto* Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			if (!Objects) return;
			if (World->GetNetMode() == NM_Client) TickClient(*World, *Objects); else TickServer(*World, *Objects);
			if (!bDone && World->GetTimeSeconds() > 180.0) Fail(TEXT("timeout"));
		}

		FDelegateHandle TickHandle, PageHandle, ActivationHandle, SettlementHandle, PresentationHandle;
		TWeakObjectPtr<UWorld> ObservedWorld;
		TWeakObjectPtr<UMeteorWorldSubsystem> ServerMeteor;
		TWeakObjectPtr<UMeteorClientWorldSubsystem> ClientMeteor;
		TMap<uint32, FNearLane> NearLanes;
		TSet<FWorldChunkCoord> SourceChunks;
		FWorldEntityId PickupId;
		FVector Origin = FVector::ZeroVector;
		FVector LastLocation = FVector::ZeroVector;
		double StartTime = 0.0, MaxSettlementDelay = 0.0, MaxClientDelay = 0.0, FirstCheckedTime = -1.0;
		double MaxBackwardCorrection = 0.0;
		uint64 LastSettlementFrame = 0;
		int32 CreatedSources = 0, FrameSettlements = 0, MaxFrameSettlements = 0, CheckedCount = 0;
		bool bOriginSet = false, bDone = false, bMovementStarted = false;
	};

	FMeteorNearFieldNetworkProbe GMeteorNearFieldNetworkProbe;
}

#endif
