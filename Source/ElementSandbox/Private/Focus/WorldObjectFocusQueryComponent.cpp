#include "Focus/WorldObjectFocusQueryComponent.h"

#include "Characters/ElementSandboxCharacter.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Focus/FocusHostComponent.h"
#include "Focus/WorldObjectFocusHandler.h"
#include "Focus/WorldObjectFocusTarget.h"
#include "GameFramework/PlayerController.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"
#include "WorldObjects/WorldObjectPickupResolver.h"
#include "WorldObjects/WorldObjectPickupComponent.h"

UWorldObjectFocusQueryComponent::UWorldObjectFocusQueryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	Handler = CreateDefaultSubobject<UWorldObjectFocusHandler>(
		TEXT("WorldObjectFocusHandler"));
}

void UWorldObjectFocusQueryComponent::BeginPlay()
{
	Super::BeginPlay();
	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController
		|| !PlayerController->IsLocalController()
		|| GetNetMode() == NM_DedicatedServer
		|| !IsValid(Handler))
	{
		return;
	}

	if (UFocusHostComponent* FocusHost =
		PlayerController->FindComponentByClass<UFocusHostComponent>())
	{
		RegistrationHandle = FocusHost->RegisterQuery(
			*this,
			FFocusQueryDelegate::CreateUObject(
				this,
				&UWorldObjectFocusQueryComponent::RunQuery),
			*Handler);
	}
}

void UWorldObjectFocusQueryComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (RegistrationHandle.IsSet())
	{
		if (UFocusHostComponent* FocusHost = GetOwner()
			? GetOwner()->FindComponentByClass<UFocusHostComponent>()
			: nullptr)
		{
			FocusHost->UnregisterQuery(RegistrationHandle);
		}
		RegistrationHandle = {};
	}
	Super::EndPlay(EndPlayReason);
}

void UWorldObjectFocusQueryComponent::RunQuery(
	const FFocusQueryContext& Context,
	TArray<FFocusQueryHit>& OutHits) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_Focus_Query);
	CandidateScratch.Reset();
	if (!Context.IsValid())
	{
		return;
	}

	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	const AElementSandboxCharacter* Character = PlayerController
		? Cast<AElementSandboxCharacter>(PlayerController->GetPawn())
		: nullptr;
	const double FocusDistance = Character ? Character->GetFocusDistance() : 0.0;
	UWorld* World = GetWorld();
	const UWorldObjectWorldSubsystem* WorldObjects = World
		? World->GetSubsystem<UWorldObjectWorldSubsystem>()
		: nullptr;
	const UWorldObjectItemCatalogSubsystem* Catalog = World
		? World->GetSubsystem<UWorldObjectItemCatalogSubsystem>()
		: nullptr;
	if (!Character || FocusDistance <= 0.0 || !WorldObjects || !Catalog)
	{
		return;
	}

	const FVector UnitDirection = Context.ViewDirection.GetSafeNormal();
	const FVector CharacterLocation = Character->GetActorLocation();
	FVector HorizontalDirection = UnitDirection.GetSafeNormal2D();
	if (HorizontalDirection.IsNearlyZero()) HorizontalDirection = Character->GetActorForwardVector().GetSafeNormal2D();
	const double MaxRayDistance = FVector::Distance(Context.ViewOrigin, CharacterLocation) + FocusDistance;
	WorldObjects->QueryPortableOverlap(FBox::BuildAABB(CharacterLocation, FVector(FocusDistance)),
		SpatialQueryScratch, BroadphaseEntities);

	const auto* PickupInput = PlayerController->FindComponentByClass<UWorldObjectPickupComponent>();
	const auto* FocusHost = PlayerController->FindComponentByClass<UFocusHostComponent>();
	const FFocusQueryHit* CurrentHit = FocusHost ? FocusHost->GetFocusedHit() : nullptr;
	const auto* CurrentTarget = CurrentHit ? CurrentHit->Target.GetPtr<FWorldObjectFocusTarget>() : nullptr;
	const FWorldEntityId CurrentId = CurrentTarget ? CurrentTarget->WorldEntityId : FWorldEntityId();
	CandidateScratch.Reserve(BroadphaseEntities.Num());
	for (const FWorldObjectEntityHandle Entity : BroadphaseEntities)
	{
		const FWorldEntityId Id = WorldObjects->GetWorldEntityId(Entity);
		UE::ElementSandbox::FWorldObjectPickupResolution Pickup;
		if (!Id.IsSet() || (PickupInput && PickupInput->IsTargetUnavailable(Id))
			|| !UE::ElementSandbox::TryResolveWorldObjectPickup(*WorldObjects, Entity, *Catalog, Pickup)) continue;
		const FVector ReachPoint = Pickup.ClosestInteractionPoint(CharacterLocation);
		const FVector ToSurface = ReachPoint - CharacterLocation;
		const double ReachDistance = ToSurface.Size();
		if (ReachDistance > FocusDistance) continue;

		double RayDistance = 0.0;
		const bool bDirectAim = Pickup.RaycastInteractionBounds(
			Context.ViewOrigin, UnitDirection, MaxRayDistance, RayDistance);
		const FVector PlanarDirection = ToSurface.GetSafeNormal2D();
		const double Facing = PlanarDirection.IsNearlyZero() ? 1.0
			: FMath::Clamp(FVector::DotProduct(HorizontalDirection, PlanarDirection), -1.0, 1.0);
		const bool bAtFeet = ToSurface.SizeSquared2D() <= FMath::Square(100.0)
			&& FMath::Abs(ToSurface.Z) <= 150.0;
		// 相机俯仰不缩窄水平辅助扇区；直接命中仍可明确指定扇区外的物件。
		if (!bDirectAim && !bAtFeet && Facing < 0.5) continue;
		if (FVector::DotProduct(UnitDirection, Pickup.ClosestInteractionPoint(Context.ViewOrigin) - Context.ViewOrigin) < 0.0)
			continue;

		FCandidate& Candidate = CandidateScratch.AddDefaulted_GetRef();
		Candidate.Pickup = MoveTemp(Pickup);
		Candidate.WorldEntityId = Id;
		Candidate.bDirectAim = bDirectAim;
		Candidate.Score = 0.65 * (1.0 - Facing) + 0.35 * ReachDistance / FocusDistance;
		Candidate.Location = bDirectAim ? Context.ViewOrigin + UnitDirection * RayDistance : ReachPoint;
		Candidate.Distance = bDirectAim ? RayDistance : FVector::Distance(Context.ViewOrigin, ReachPoint);
	}
	CandidateScratch.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.bDirectAim != B.bDirectAim) return A.bDirectAim;
		const double AScore = A.bDirectAim ? A.Distance : A.Score;
		const double BScore = B.bDirectAim ? B.Distance : B.Score;
		return AScore != BScore ? AScore < BScore : A.WorldEntityId.GetValue() < B.WorldEntityId.GetValue();
	});
	const auto IsAccessible = [&](const FCandidate& Candidate)
	{
		return UE::ElementSandbox::HasClearWorldObjectPickupPath(*World, *Character, Candidate.Pickup, CharacterLocation)
			&& UE::ElementSandbox::HasClearWorldObjectPickupPath(*World, *Character, Candidate.Pickup, Context.ViewOrigin);
	};
	const FCandidate* Best = CandidateScratch.FindByPredicate(IsAccessible);
	if (Best && !Best->bDirectAim && Best->WorldEntityId != CurrentId)
	{
		const FCandidate* Current = CandidateScratch.FindByPredicate(
			[CurrentId](const FCandidate& Candidate) { return Candidate.WorldEntityId == CurrentId; });
		if (Current && Best->Score + 0.12 >= Current->Score && IsAccessible(*Current)) Best = Current;
	}
	if (Best)
	{
		FWorldObjectFocusTarget Target;
		Target.WorldEntityId = Best->WorldEntityId;
		FFocusQueryHit& Hit = OutHits.AddDefaulted_GetRef();
		Hit.HitDistance = Best->Distance;
		Hit.HitLocation = Best->Location;
		Hit.bDirectAim = Best->bDirectAim;
		Hit.SelectionScore = Best->Score;
		Hit.bRepeatableInteract = true;
		Hit.Target = FInstancedStruct::Make<FWorldObjectFocusTarget>(Target);
	}
	CandidateScratch.Reset();
}
