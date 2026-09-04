#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ElementSandboxCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Focus/FocusHostComponent.h"
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
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupComponent.h"

namespace
{
	/** 显式测试开关、独立空存档：真实双客户端抢同一运动物件，然后长按收集余下三件。 */
	class FNearbyPickupNetworkProbe final
	{
	public:
		FNearbyPickupNetworkProbe()
		{
			bPreview = FParse::Param(FCommandLine::Get(), TEXT("NearbyPickupPreview"));
			if (bPreview || FParse::Param(FCommandLine::Get(), TEXT("NearbyPickupNetworkProbe")))
				TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FNearbyPickupNetworkProbe::Tick);
		}
		~FNearbyPickupNetworkProbe() { FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle); }

	private:
		static int32 CountWood(const AElementSandboxPlayerController& Controller)
		{
			const auto* State = Controller.GetPlayerState<AElementSandboxPlayerState>();
			const auto* Inventory = State ? State->GetInventoryComponent() : nullptr;
			const auto* Catalog = Controller.GetWorld()->GetSubsystem<UWorldObjectItemCatalogSubsystem>();
			const auto* Definition = Catalog ? Catalog->FindItemDefinition(GetDefault<UWoodBlockWorldObjectDefinition>()) : nullptr;
			int32 Count = 0;
			if (Inventory && Definition)
				for (const UItemInstance* Item : Inventory->GetBackpackSlots())
					if (Item && Item->GetDefinition().GetObject() == Definition)
						if (const auto* Stack = Item->FindFeature<UItemStackFeature>()) Count += Stack->GetQuantity();
			return Count;
		}

		void Fail(const TCHAR* Reason)
		{
			UE_LOG(LogTemp, Error, TEXT("NearbyPickupProbe: FAIL %s"), Reason);
			bDone = true;
		}

		void TickServer(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			TArray<AElementSandboxPlayerController*> Players;
			for (auto It = World.GetPlayerControllerIterator(); It; ++It)
				if (auto* Player = Cast<AElementSandboxPlayerController>(It->Get()))
					if (Player->GetPawn() && Player->PlayerState) Players.Add(Player);
			if (Players.Num() < (bPreview ? 1 : 2) || World.GetTimeSeconds() < 10.0) return;
			Players.Sort([](const auto& A, const auto& B) { return A.PlayerState->GetPlayerId() < B.PlayerState->GetPlayerId(); });
			if (!bPositioned)
			{
				Origin = Players[0]->GetPawn()->GetActorLocation();
				for (int32 Index = 0; Index < Players.Num(); ++Index)
				{
					const FVector Position = Origin + FVector(0, Index * 160.0, 0);
					Players[Index]->GetPawn()->TeleportTo(Position, FRotator::ZeroRotator);
					Players[Index]->ClientSetLocation(Position, FRotator::ZeroRotator);
				}
				bPositioned = true;
			}
			if (Ids.IsEmpty())
			{
				if (!bPreview)
					for (const auto* Player : Players)
					{
						const auto* Streaming = Player->FindComponentByClass<UWorldChunkStreamingComponent>();
						if (!Streaming || !Streaming->IsAuthorityChunkReadyForLiveMutation(
							FWorldChunkCoord::FromWorldLocation(Origin + FVector(200, 80, 0)))) return;
					}
				for (int32 Index = 0; Index < (bPreview ? 8 : 4); ++Index)
				{
					FWorldObjectCreateDesc Desc;
					Desc.Definition = Objects.FindDefinition(GetDefault<UWoodBlockWorldObjectDefinition>()->DefinitionId);
					if (!Desc.Definition) { Fail(TEXT("wood definition")); return; }
					Desc.WorldTransform.SetLocation(FVector(Origin.X + 170 + (Index % 4) * 25,
						Origin.Y + (Index % 2 == 0 ? 35 : 100), Index == 0 ? 160 : 20));
					if (Index == 0)
					{
						Desc.MotionState = EWorldObjectMotionState::Physics;
						Desc.InstanceInteractionBounds = Desc.Definition->InteractionLocalBounds;
						FWorldObjectPhysicsBodyInit Body;
						Body.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
						Desc.PhysicsBody = Body;
					}
					const auto Entity = Objects.CreateEntity(Desc);
					const auto Id = Objects.GetWorldEntityId(Entity);
					if (!Id.IsSet()) { Fail(TEXT("fixture creation")); return; }
					Ids.Add(Id);
					if (Index == 0)
					{
						const auto* Proxy = Objects.GetProxy(Entity);
						auto* Actor = Proxy ? Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()) : nullptr;
						if (!Actor) { Fail(TEXT("physical fixture proxy")); return; }
						Actor->ReleasePhysicsImmediately();
						Actor->GetPhysicsBox()->SetEnableGravity(bPreview);
						Actor->GetPhysicsBox()->SetAngularDamping(0.0f);
						Actor->GetPhysicsBox()->SetPhysicsAngularVelocityInDegrees(FVector(0, 0, 90));
					}
				}
				Objects.OnEntityPreDestroy().AddLambda([this, &Objects](const FWorldObjectEntityHandle Entity, bool&)
				{
					if (Objects.GetWorldEntityId(Entity) != Ids[0]) return;
					const auto* Motion = Objects.GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity);
					bPickedWhilePhysics = Motion && Motion->State == EWorldObjectMotionState::Physics;
				});
				UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: CREATED count=%d moving=%llu preview=%d"), Ids.Num(), Ids[0].GetValue(), bPreview);
				if (bPreview) bDone = true;
				return;
			}
			if (Ids.ContainsByPredicate([&Objects](const FWorldEntityId Id) { return Objects.FindEntity(Id).IsSet(); })) return;
			const int32 FirstCount = CountWood(*Players[0]), SecondCount = CountWood(*Players[1]);
			if (!bPickedWhilePhysics || FirstCount + SecondCount != 4 || FirstCount < 3 || SecondCount > 1)
			{ Fail(TEXT("authoritative physics pickup and exactly four total items")); return; }
			UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: PASS server physics_at_pickup=1 first=%d second=%d total=4"), FirstCount, SecondCount);
			bDone = true;
		}

		void TickClient(UWorld& World, UWorldObjectWorldSubsystem& Objects)
		{
			auto* Player = Cast<AElementSandboxPlayerController>(World.GetFirstPlayerController());
			auto* State = World.GetGameState();
			auto* Products = World.GetSubsystem<UWoodProductPresentationWorldSubsystem>();
			if (!Player || !Player->GetPawn() || !Player->PlayerState || !State || State->PlayerArray.Num() < 2 || !Products) return;
			int32 Picker = MAX_int32;
			for (const APlayerState* Entry : State->PlayerArray) if (Entry) Picker = FMath::Min(Picker, Entry->GetPlayerId());
			const bool bPicker = Player->PlayerState->GetPlayerId() == Picker;
			const double Now = World.GetTimeSeconds();
			if (Ids.IsEmpty())
			{
				FWorldObjectSpatialQueryScratch Scratch;
				TArray<FWorldObjectEntityHandle> Nearby;
				Objects.QueryPortableOverlap(FBox::BuildAABB(Player->GetPawn()->GetActorLocation(), FVector(2000)), Scratch, Nearby);
				for (const auto Entity : Nearby)
				{
					const auto* Definition = Objects.GetRegistry().FindFragment<FWorldObjectDefinitionFragment>(Entity);
					if (Definition && Definition->Definition.Get() == GetDefault<UWoodBlockWorldObjectDefinition>())
						Ids.Add(Objects.GetWorldEntityId(Entity));
				}
				if (Ids.Num() != 4) { Ids.Reset(); return; }
				Ids.Sort();
				ObservedAt = Now;
				UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: OBSERVED picker=%d moving=%llu"), bPicker, Ids[0].GetValue());
			}
			if (!bRaceRequested && Now - ObservedAt > 1.0)
			{
				if (!Player->RequestPickupWorldObject(Ids[0]) || Player->RequestPickupWorldObject(Ids[0]))
				{ Fail(TEXT("one in-flight request, duplicate must be suppressed")); return; }
				bRaceRequested = true;
				UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: RACE_REQUEST picker=%d duplicate_suppressed=1"), bPicker);
			}
			auto* Input = Player->FindComponentByClass<UWorldObjectPickupComponent>();
			if (bPicker && bRaceRequested && !bHoldStarted && Now - ObservedAt > 3.0)
			{
				auto* Focus = Player->FindComponentByClass<UFocusHostComponent>();
				Player->SetControlRotation(FRotator::ZeroRotator);
				FFocusQueryContext Context;
				FRotator Rotation;
				Player->GetPlayerViewPoint(Context.ViewOrigin, Rotation);
				Context.ViewDirection = FVector::ForwardVector;
				Focus->EvaluateFocus(Context);
				const auto* Hit = Focus->GetFocusedHit();
				if (!Hit || Hit->bDirectAim || !Hit->bRepeatableInteract || !Input->BeginInteract() || !Input->IsCollecting())
				{ Fail(TEXT("horizontal view must start assisted hold pickup")); return; }
				bHoldStarted = true;
				UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: HOLD_STARTED assisted=1"));
			}
			if (!bRaceRequested || (bPicker && !bHoldStarted) || Now - ObservedAt < 5.0) return;
			for (const auto Id : Ids)
			{
				UHierarchicalInstancedStaticMeshComponent* Mesh = nullptr;
				int32 Index = INDEX_NONE;
				if (Objects.FindEntity(Id).IsSet() || Products->FindInstance(Id, Mesh, Index)) return;
			}
			const int32 Count = CountWood(*Player);
			if ((bPicker && (Count < 3 || Count > 4)) || (!bPicker && Count > 1))
			{ Fail(TEXT("owner inventory replication")); return; }
			Input->EndInteract();
			UE_LOG(LogTemp, Display, TEXT("NearbyPickupProbe: PASS client picker=%d count=%d ecs_removed=4 hism_removed=4 collecting=%d"), bPicker, Count, Input->IsCollecting());
			bDone = true;
		}

		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld()) return;
			if (!ObservedWorld.IsValid()) ObservedWorld = World;
			if (ObservedWorld != World) return;
			auto* Objects = World->GetSubsystem<UWorldObjectWorldSubsystem>();
			if (!Objects) return;
			if (World->GetNetMode() == NM_Client) { if (!bPreview) TickClient(*World, *Objects); }
			else if (bPreview || World->GetNetMode() == NM_DedicatedServer) TickServer(*World, *Objects);
			if (!bDone && World->GetTimeSeconds() > 70.0) Fail(TEXT("timeout"));
		}

		FDelegateHandle TickHandle;
		TWeakObjectPtr<UWorld> ObservedWorld;
		TArray<FWorldEntityId> Ids;
		FVector Origin = FVector::ZeroVector;
		double ObservedAt = 0.0;
		bool bPreview = false, bPositioned = false, bPickedWhilePhysics = false;
		bool bRaceRequested = false, bHoldStarted = false, bDone = false;
	};

	FNearbyPickupNetworkProbe GNearbyPickupNetworkProbe;
}

#endif
