#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ElementSandboxCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/FocusInteractionPrompt.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "Game/ElementSandboxPlayerController.h"
#include "Game/ElementSandboxPlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "Inventory/InventoryComponent.h"
#include "Item/Features/ItemStackFeature.h"
#include "Item/ItemDefinition.h"
#include "Item/ItemInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"

namespace
{
	/** 显式 -WorldObjectInteractionNetworkProbe：隔离空存档内验证真实拾取 RPC 和接触后的投影。 */
	class FWorldObjectInteractionNetworkProbe final
	{
	public:
			FWorldObjectInteractionNetworkProbe()
			{
					bWithBacklog = FParse::Param(FCommandLine::Get(), TEXT("WorldObjectInteractionBacklog"));
				bRepeatContact = FParse::Param(FCommandLine::Get(), TEXT("WorldObjectContactRepeat"));
			if (FParse::Param(FCommandLine::Get(), TEXT("WorldObjectInteractionNetworkProbe")))
				TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(
					this, &FWorldObjectInteractionNetworkProbe::Tick);
		}
		~FWorldObjectInteractionNetworkProbe() { FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle); }

	private:
		static int32 CountWood(const AElementSandboxPlayerController& Controller)
		{
			const auto* State = Controller.GetPlayerState<AElementSandboxPlayerState>();
			const auto* Inventory = State ? State->GetInventoryComponent() : nullptr;
			const auto* Catalog = Controller.GetWorld()->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
			const auto* WoodDefinition = Catalog ? Catalog->FindItemDefinition(GetDefault<UWoodBlockWorldObjectDefinition>()) : nullptr;
			if (!Inventory || !WoodDefinition) return 0;
			int32 Count = 0;
			for (const UItemInstance* Item : Inventory->GetBackpackSlots())
			{
				if (!Item || Item->GetDefinition().GetObject() != WoodDefinition) continue;
				const auto* Stack = Item->FindFeature<UItemStackFeature>();
				if (Stack) Count += Stack->GetQuantity();
			}
			return Count;
		}

		void Fail(const TCHAR* Reason)
		{
			UE_LOG(LogTemp, Error, TEXT("WorldObjectInteractionProbe: FAIL %s"), Reason);
			bDone = true;
		}

			bool IsDormant(const UWorldObjectWorldSubsystem& Objects, FWorldEntityId Id) const
		{
			const auto* Motion = Objects.GetRegistry().FindFragment<FWorldObjectMotionFragment>(Objects.FindEntity(Id));
			return Motion && Motion->State == EWorldObjectMotionState::Dormant;
			}

			bool PrepareBacklog(UWorldObjectWorldSubsystem& Objects,
				TConstArrayView<AElementSandboxPlayerController*> Players)
			{
				if (!bWithBacklog || bBacklogQueued) return true;
				constexpr int32 BackgroundObjectCount = 24576;
				constexpr int32 ObjectsPerChunk = 1536;
				if (BackgroundEntities.Num() < BackgroundObjectCount)
				{
					TArray<FWorldObjectCreateDesc> Descs;
					for (int32 Offset = 0; Offset < 512; ++Offset)
					{
						const int32 Index = BackgroundEntities.Num() + Offset;
						const FWorldChunkCoord Coord(-12 + Index / ObjectsPerChunk, 8, 0);
						FWorldObjectCreateDesc& Desc = Descs.AddDefaulted_GetRef();
						Desc.Definition = Objects.FindDefinition(GetDefault<UWoodBlockWorldObjectDefinition>()->DefinitionId);
						Desc.WorldTransform.SetLocation(Coord.GetWorldMinimum() + FVector(
							100 + (Index % 32) * 280, 100 + (Index / 32 % 48) * 180, 20));
					}
					FWorldObjectStagedCreateBatch Staged;
					TArray<FWorldObjectEntityHandle> Created;
					if (!Objects.StageCreateEntities(Descs, Staged) || !Objects.CommitStagedCreateEntities(Staged, Created))
					{
						Objects.RollbackStagedCreateEntities(Staged);
						Fail(TEXT("backlog fixture creation")); return false;
					}
					BackgroundEntities.Append(Created);
					return false;
				}
				// 先让两端收到正式 Snapshot，之后的清图才确实进入 Live Delta 队列。
				for (const auto* Player : Players)
				{
					const auto* Streaming = Player->FindComponentByClass<UWorldChunkStreamingComponent>();
					if (!Streaming || Streaming->GetStreamingStats().PendingLiveDeltaCount != 0) return false;
					for (int32 Index = 0; Index < BackgroundObjectCount / ObjectsPerChunk; ++Index)
						if (!Streaming->IsAuthorityChunkReadyForLiveMutation(FWorldChunkCoord(-12 + Index, 8, 0))) return false;
				}
				if (!Objects.BeginGameplayDestructionBatch()) { Fail(TEXT("begin background destruction")); return false; }
				bool bDestroyed = true;
				for (const auto Entity : BackgroundEntities) bDestroyed &= Objects.DestroyEntity(Entity);
				bDestroyed &= Objects.EndGameplayDestructionBatch();
				if (!bDestroyed) { Fail(TEXT("background destruction")); return false; }
				bBacklogQueued = true;
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: BACKLOG queued=%d per_client"), BackgroundEntities.Num());
				return true;
			}

			void TickServer(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			TArray<AElementSandboxPlayerController*> Players;
			for (auto It = World.GetPlayerControllerIterator(); It; ++It)
			{
				auto* Controller = Cast<AElementSandboxPlayerController>(It->Get());
				if (Controller && Controller->GetPawn() && Controller->PlayerState) Players.Add(Controller);
			}
			if (Players.Num() < 2) return;
			Players.Sort([](const auto& A, const auto& B) { return A.PlayerState->GetPlayerId() < B.PlayerState->GetPlayerId(); });
			const double Now = World.GetTimeSeconds();
			if (!PickupId.IsSet())
			{
					if (Now < 10.0) return;
						if (!PrepareBacklog(Objects, Players)) return;
					PawnStart = Players[0]->GetPawn()->GetActorLocation();
					// 两端完成出生区 Snapshot 后再创建夹具，避免把入场同步竞态当作接触失败。
					for (const auto* Player : Players)
					{
						const auto* Streaming = Player->FindComponentByClass<UWorldChunkStreamingComponent>();
						if (!Streaming || !Streaming->IsAuthorityChunkReadyForLiveMutation(
							FWorldChunkCoord::FromWorldLocation(PawnStart + FVector(800, 0, 0)))) return;
					}
				FWorldObjectCreateDesc Desc;
				Desc.Definition = Objects.FindDefinition(GetDefault<UWoodBlockWorldObjectDefinition>()->DefinitionId);
				Desc.WorldTransform.SetLocation(FVector(PawnStart.X + 335.0, PawnStart.Y, 20.0));
				PickupId = Objects.GetWorldEntityId(Objects.CreateEntity(Desc));
					Desc.WorldTransform.SetLocation(FVector(PawnStart.X + 800.0, PawnStart.Y, bWithBacklog ? 20.0 : 120.0));
					Desc.MotionState = bWithBacklog ? EWorldObjectMotionState::Dormant : EWorldObjectMotionState::Physics;
				Desc.InstanceInteractionBounds = Desc.Definition->InteractionLocalBounds;
				FWorldObjectPhysicsBodyInit Physics;
				Physics.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
				Physics.MassKg = 4.0f;
					if (!bWithBacklog) Desc.PhysicsBody = Physics;
				ContactId = Objects.GetWorldEntityId(Objects.CreateEntity(Desc));
				if (!PickupId.IsSet() || !ContactId.IsSet()) { Fail(TEXT("source creation")); return; }
				StartTime = Now;
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: CREATED pickup=%llu contact=%llu pawn=(%.2f,%.2f,%.2f)"),
					PickupId.GetValue(), ContactId.GetValue(), PawnStart.X, PawnStart.Y, PawnStart.Z);
			}
				const bool bContactDormant = IsDormant(Objects, ContactId);
					if (bWasContactDormant != bContactDormant)
						UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: SERVER_STATE dormant=%d elapsed=%.3f"),
							bContactDormant, Now - StartTime);
					if (bWasContactDormant && !bContactDormant) ++ContactWakeCount;
				bWasContactDormant = bContactDormant;
			bInitiallyRested |= bContactDormant;
				if (!Objects.FindEntity(PickupId).IsSet())
				{
					if (bWithBacklog && !bPickupRemoved)
					{
						const auto* Streaming = Players[0]->FindComponentByClass<UWorldChunkStreamingComponent>();
						const int32 Pending = Streaming ? Streaming->GetStreamingStats().PendingLiveDeltaCount : 0;
						UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: NEAR_READY elapsed=%.3fs background_pending=%d"), Now - StartTime, Pending);
						if (Pending < 1024 || Now - StartTime > 3.0)
						{
							Fail(TEXT("near pickup must complete while far destruction remains queued")); return;
						}
					}
				bPickupRemoved = true;
				bContactWoke |= bInitiallyRested && !bContactDormant;
			}
			if (Now - StartTime < 55.0) return;
			const auto* Transform = Objects.GetRegistry().FindFragment<FWorldObjectTransformFragment>(Objects.FindEntity(ContactId));
			if (!bPickupRemoved || CountWood(*Players[0]) != 1 || CountWood(*Players[1]) != 0)
			{
				Fail(TEXT("server pickup must grant exactly one item to the requesting owner")); return;
			}
			if (!bInitiallyRested || !bContactWoke || !bContactDormant || !Transform
					|| (bRepeatContact ? ContactWakeCount < 3
						: Transform->WorldTransform.GetLocation().X < PawnStart.X + 850.0
							|| Players[0]->GetPawn()->GetActorLocation().X < PawnStart.X + 1000.0))
			{
				Fail(TEXT("server contact must wake, move and settle under the same world identity")); return;
			}
			const FVector Position = Transform->WorldTransform.GetLocation();
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: PASS server pickup=%llu contact=%llu final=(%.3f,%.3f,%.3f) wakes=%d"),
					PickupId.GetValue(), ContactId.GetValue(), Position.X, Position.Y, Position.Z, ContactWakeCount);
			bDone = true;
		}

			void DriveRepeatedContact(AElementSandboxCharacter& Character,
				UWorldObjectWorldSubsystem& Objects, const double Now, const bool bDormant)
			{
				const auto* Transform = Objects.GetRegistry().FindFragment<FWorldObjectTransformFragment>(Objects.FindEntity(ContactId));
				if (!Transform || CompletedContactCycles >= 3) return;
				if (ContactPhaseStarted == 0.0) ContactPhaseStarted = Now;
				const FVector ToBlock = (Transform->WorldTransform.GetLocation() - Character.GetActorLocation()).GetSafeNormal2D();
				FVector Input = FVector::ZeroVector;
				if (ContactPhase == 0)
				{
					if (Now - ContactPhaseStarted < 4.0) Input = ToBlock;
					else
					{
						ContactPhase = 1; ContactPhaseStarted = Now;
						RetreatDirection = ToBlock.IsNearlyZero() ? -FVector::ForwardVector : -ToBlock;
					}
				}
				if (ContactPhase == 1)
				{
					if (Now - ContactPhaseStarted < 1.0) Input = RetreatDirection;
					else { ContactPhase = 2; ContactPhaseStarted = Now; }
				}
				if (ContactPhase == 2 && bDormant && Now - ContactPhaseStarted >= 2.0)
				{
					++CompletedContactCycles;
					UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: CONTACT_CYCLE completed=%d max_backward_cm=%.3f"),
						CompletedContactCycles, MaxBackwardCorrection);
					ContactPhase = 0; ContactPhaseStarted = Now;
				}
				if (!LastInputDirection.IsNearlyZero())
				{
					const FVector Displacement = Character.GetActorLocation() - LastPawnLocation;
					const double Backward = -FVector::DotProduct(Displacement, LastInputDirection);
					if (Backward > MaxBackwardCorrection)
					{
						MaxBackwardCorrection = Backward;
						if (Backward > 10.0)
							UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: BACKSTEP cm=%.3f elapsed=%.3f phase=%d dt=%.4f pawn=%s delta=%s velocity=%s input=%s next=%s block=%s"),
								Backward, Now - StartTime, ContactPhase, Character.GetWorld()->GetDeltaSeconds(),
								*Character.GetActorLocation().ToCompactString(), *Displacement.ToCompactString(),
								*Character.GetVelocity().ToCompactString(), *LastInputDirection.ToCompactString(),
								*Input.ToCompactString(), *Transform->WorldTransform.GetLocation().ToCompactString());
					}
				}
				if (!Input.IsNearlyZero()) Character.AddMovementInput(Input, 1.0f, true);
				LastInputDirection = Input;
			}

			void TickClient(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			auto* Controller = Cast<AElementSandboxPlayerController>(World.GetFirstPlayerController());
			auto* Character = Controller ? Cast<AElementSandboxCharacter>(Controller->GetPawn()) : nullptr;
			auto* State = World.GetGameState();
			auto* Wood = World.GetSubsystem<UWoodProductPresentationWorldSubsystem>();
			if (!Character || !Controller->PlayerState || !State || State->PlayerArray.Num() < 2 || !Wood) return;
			int32 Picker = MAX_int32;
			for (const APlayerState* Player : State->PlayerArray)
				if (Player) Picker = FMath::Min(Picker, Player->GetPlayerId());
			const bool bPicker = Picker == Controller->PlayerState->GetPlayerId();
			const double Now = World.GetTimeSeconds();
			if (!PickupId.IsSet())
			{
				FWorldObjectSpatialQueryScratch Scratch;
				TArray<FWorldObjectEntityHandle> Nearby;
				Objects.QueryOverlap(FBox::BuildAABB(Character->GetActorLocation(), FVector(2500.0)), Scratch, Nearby);
				TArray<FWorldEntityId> Ids;
				for (const auto Entity : Nearby)
				{
					const auto* Definition = Objects.GetRegistry().FindFragment<FWorldObjectDefinitionFragment>(Entity);
					if (Definition && Definition->Definition.Get() == GetDefault<UWoodBlockWorldObjectDefinition>())
						Ids.Add(Objects.GetWorldEntityId(Entity));
				}
				if (Ids.Num() != 2) return;
				Ids.Sort();
				PickupId = Ids[0]; ContactId = Ids[1];
				PawnStart = Character->GetActorLocation();
				StartTime = Now;
				LastPawnLocation = PawnStart;
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: OBSERVED picker=%d pickup=%llu contact=%llu"),
					bPicker, PickupId.GetValue(), ContactId.GetValue());
			}
				const bool bContactDormant = IsDormant(Objects, ContactId);
				bInitiallyRested |= bContactDormant;
				if (const auto* Proxy = Objects.GetProxy(Objects.FindEntity(ContactId)))
				{
					auto* PhysicsActor = Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner());
					if (PhysicsActor && PhysicsActor->HasClientPhysicsProjection()
						&& PhysicsActor->GetPhysicsBox()->IsSimulatingPhysics()
						&& PhysicsActor->GetPhysicsReplicationMode() == EPhysicsReplicationMode::PredictiveInterpolation)
						++PredictedPhysicsFrames;
				}
			const auto Pickup = Objects.FindEntity(PickupId);
			UHierarchicalInstancedStaticMeshComponent* Component = nullptr;
			int32 Index = INDEX_NONE;
				if (bPicker && !bRequested && Now - StartTime > (bWithBacklog ? 0.1 : 10.0) && bInitiallyRested)
			{
				const auto* Transform = Objects.GetRegistry().FindFragment<FWorldObjectTransformFragment>(Pickup);
				auto* Focus = Controller->FindComponentByClass<UFocusHostComponent>();
					if (!Transform || !Focus || !Wood->FindInstance(PickupId, Component, Index)) return;
					if (bWithBacklog)
					{
						const FVector Center = Transform->WorldTransform.GetLocation();
						FHitResult CollisionHit;
						const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
						FCollisionQueryParams Query(SCENE_QUERY_STAT(NearDebrisPawn), false, Character);
						const FVector SweepCenter(Center.X, Center.Y, Capsule->GetScaledCapsuleHalfHeight() + 2.0);
						if (!World.SweepSingleByChannel(CollisionHit,
							SweepCenter - FVector(200, 0, 0), SweepCenter + FVector(200, 0, 0), FQuat::Identity,
							Capsule->GetCollisionObjectType(), Capsule->GetCollisionShape(), Query,
							FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()))
							|| !CollisionHit.bBlockingHit || CollisionHit.Item == INDEX_NONE)
						{
							Fail(TEXT("near settled object must block the actual client Pawn capsule")); return;
						}
						UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: PAWN_BLOCK hit_distance=%.2f"), CollisionHit.Distance);
					}
				FFocusQueryContext Context;
				FRotator ViewRotation;
				Controller->GetPlayerViewPoint(Context.ViewOrigin, ViewRotation);
				Context.ViewDirection = (Transform->WorldTransform.GetLocation() - Context.ViewOrigin).GetSafeNormal();
				Focus->EvaluateFocus(Context);
				const auto* Hit = Focus->GetFocusedHit();
				const auto* Target = Hit ? Hit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
				FFocusInteractionPrompt Prompt;
				if (!Target || Target->WorldEntityId != PickupId || !Focus->TryResolveFocusedPrompt(Prompt)
					|| FVector::Distance(Character->GetActorLocation(), Transform->WorldTransform.GetLocation())
						<= Character->GetFocusDistance())
				{
					Fail(TEXT("client must display the real pickup prompt while the center is beyond reach")); return;
				}
				if (!Focus->HandleInteract()) { Fail(TEXT("focused E interaction")); return; }
				Controller->RequestPickupWorldObject(PickupId);
				bRequested = true;
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: REQUEST prompt=%s center_distance=%.2f"),
					*Prompt.Text.ToString(), FVector::Distance(Character->GetActorLocation(), Transform->WorldTransform.GetLocation()));
			}
			if (!Pickup.IsSet() && !Wood->FindInstance(PickupId, Component, Index))
			{
				bPickupRemoved = true;
				bContactWoke |= bInitiallyRested && !bContactDormant;
			}
				if (bPicker && bPickupRemoved && bRepeatContact)
					DriveRepeatedContact(*Character, Objects, Now, bContactDormant);
				else if (bPicker && bPickupRemoved && Now - StartTime < 25.0
				&& Character->GetActorLocation().X < PawnStart.X + 1050.0)
			{
				Character->AddMovementInput(FVector::ForwardVector, 1.0f, true);
				MaxBackwardCorrection = FMath::Max(MaxBackwardCorrection,
					LastPawnLocation.X - Character->GetActorLocation().X);
			}
			LastPawnLocation = Character->GetActorLocation();
			if (Now - StartTime < 50.0) return;
			const auto* Transform = Objects.GetRegistry().FindFragment<FWorldObjectTransformFragment>(Objects.FindEntity(ContactId));
			FTransform Render;
			if (!bPickupRemoved || CountWood(*Controller) != (bPicker ? 1 : 0)
				|| !bInitiallyRested || !bContactWoke || !bContactDormant || !Transform
					|| !Wood->FindInstance(ContactId, Component, Index)
					|| !Component->GetInstanceTransform(Index, Render, true)
					|| !Render.Equals(Transform->WorldTransform, 0.1)
					|| (bRepeatContact && (PredictedPhysicsFrames < 10
						|| (bPicker && (CompletedContactCycles != 3 || MaxBackwardCorrection > 20.0)))))
				{
					UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: CLIENT_STATE picker=%d removed=%d rested=%d woke=%d dormant=%d predicted_frames=%d cycles=%d phase=%d max_backward_cm=%.3f"),
						bPicker, bPickupRemoved, bInitiallyRested, bContactWoke, bContactDormant,
						PredictedPhysicsFrames, CompletedContactCycles, ContactPhase, MaxBackwardCorrection);
					Fail(TEXT("client pickup removal or final ECS/HISM contact handoff")); return;
			}
			const FVector Position = Transform->WorldTransform.GetLocation();
				UE_LOG(LogTemp, Display, TEXT("WorldObjectInteractionProbe: PASS client picker=%d pickup=%llu contact=%llu final=(%.3f,%.3f,%.3f) max_backward_cm=%.3f predicted_frames=%d cycles=%d"),
					bPicker, PickupId.GetValue(), ContactId.GetValue(), Position.X, Position.Y, Position.Z,
					MaxBackwardCorrection, PredictedPhysicsFrames, CompletedContactCycles);
			bDone = true;
		}

		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld()
				|| (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_DedicatedServer)) return;
			if (!ObservedWorld.IsValid()) ObservedWorld = World;
			if (ObservedWorld != World) return;
			auto* Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			if (!Objects) return;
			if (World->GetNetMode() == NM_Client) TickClient(*World, *Objects); else TickServer(*World, *Objects);
				if (!bDone && World->GetTimeSeconds() > (bWithBacklog ? 160.0 : 100.0)) Fail(TEXT("timeout"));
		}

		FDelegateHandle TickHandle;
		TWeakObjectPtr<UWorld> ObservedWorld;
			FWorldEntityId PickupId, ContactId;
			TArray<FWorldObjectEntityHandle> BackgroundEntities;
		FVector PawnStart = FVector::ZeroVector, LastPawnLocation = FVector::ZeroVector;
			double StartTime = 0.0, MaxBackwardCorrection = 0.0;
			double ContactPhaseStarted = 0.0;
			FVector LastInputDirection = FVector::ZeroVector, RetreatDirection = FVector::ZeroVector;
			int32 ContactPhase = 0, CompletedContactCycles = 0, ContactWakeCount = 0, PredictedPhysicsFrames = 0;
			bool bRepeatContact = false, bWasContactDormant = false;
			bool bRequested = false, bPickupRemoved = false, bInitiallyRested = false, bContactWoke = false, bDone = false;
			bool bWithBacklog = false, bBacklogQueued = false;
	};

	FWorldObjectInteractionNetworkProbe GWorldObjectInteractionNetworkProbe;
}

#endif
