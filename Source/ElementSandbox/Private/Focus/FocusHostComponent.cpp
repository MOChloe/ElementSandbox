#include "Focus/FocusHostComponent.h"

#include "ElementSandboxBuilding.h"
#include "Focus/FocusHandler.h"
#include "Focus/FocusInteractionPrompt.h"
#include "GameFramework/PlayerController.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

UFocusHostComponent::UFocusHostComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

FFocusQueryRegistrationHandle UFocusHostComponent::RegisterQuery(
	UObject& Owner,
	FFocusQueryDelegate Query,
	UFocusHandler& Handler)
{
	if (bEvaluatingQueries
		|| bDispatchingFocusChange
		|| !IsValid(&Owner)
		|| !IsValid(&Handler)
		|| !Query.IsBound())
	{
		return {};
	}

	if (NextRegistrationValue == 0)
	{
		++NextRegistrationValue;
	}

	FRegistration& Registration = Registrations.AddDefaulted_GetRef();
	Registration.Handle = FFocusQueryRegistrationHandle(NextRegistrationValue++);
	Registration.Owner = &Owner;
	Registration.Query = MoveTemp(Query);
	Registration.Handler = &Handler;
	return Registration.Handle;
}

bool UFocusHostComponent::UnregisterQuery(const FFocusQueryRegistrationHandle Handle)
{
	if (bEvaluatingQueries || bDispatchingFocusChange || !Handle.IsSet())
	{
		return false;
	}
	if (!FindRegistration(Handle))
	{
		return false;
	}

	if (FocusedRegistration == Handle)
	{
		ClearFocus();
	}

	const int32 RemovedCount = Registrations.RemoveAll(
		[Handle](const FRegistration& Registration)
		{
			return Registration.Handle == Handle;
		});

	return RemovedCount > 0;
}

void UFocusHostComponent::EvaluateFocus(const FFocusQueryContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Focus_Evaluate);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, FocusEvaluate);
	PruneInvalidRegistrations();
	if (Registrations.IsEmpty() || !Context.IsValid())
	{
		ClearFocus();
		return;
	}

	FCandidate BestCandidate;
	bool bHasBestCandidate = false;
	bEvaluatingQueries = true;
	for (const FRegistration& Registration : Registrations)
	{
		QueryHitScratch.Reset();
		Registration.Query.Execute(Context, QueryHitScratch);
		for (FFocusQueryHit& QueryHit : QueryHitScratch)
		{
			if (!QueryHit.IsValid() || (bRepeatableInteractOnly && !QueryHit.bRepeatableInteract)) continue;
			const FFocusQueryHit& BestHit = BestCandidate.Hit;
			const bool bBetter = !bHasBestCandidate
				|| (QueryHit.bDirectAim && !BestHit.bDirectAim)
				|| (QueryHit.bDirectAim == BestHit.bDirectAim
					&& (QueryHit.bDirectAim ? QueryHit.HitDistance < BestHit.HitDistance
						: QueryHit.SelectionScore < BestHit.SelectionScore
							|| (QueryHit.SelectionScore == BestHit.SelectionScore && QueryHit.HitDistance < BestHit.HitDistance)));
			if (bBetter)
			{
				BestCandidate.Registration = Registration.Handle;
				BestCandidate.Hit = MoveTemp(QueryHit);
				bHasBestCandidate = true;
			}
		}
	}
	bEvaluatingQueries = false;
	QueryHitScratch.Reset();

	if (!bHasBestCandidate)
	{
		ClearFocus();
		return;
	}

	ApplyFocus(MoveTemp(BestCandidate));
}

void UFocusHostComponent::SetRepeatableInteractOnly(const bool bEnabled)
{
	bRepeatableInteractOnly = bEnabled;
	if (bEnabled && bHasFocusedHit && !FocusedHit.bRepeatableInteract) ClearFocus();
}

void UFocusHostComponent::ClearFocus()
{
	if (!bHasFocusedHit)
	{
		FocusedRegistration = {};
		FocusedHit = {};
		return;
	}

	const FRegistration* Registration = FindRegistration(FocusedRegistration);
	UFocusHandler* Handler = Registration ? Registration->Handler.Get() : nullptr;
	FFocusQueryHit LostHit = MoveTemp(FocusedHit);
	bHasFocusedHit = false;
	FocusedRegistration = {};
	FocusedHit = {};

	if (IsValid(Handler) && !bDispatchingFocusChange)
	{
		TGuardValue<bool> DispatchGuard(bDispatchingFocusChange, true);
		Handler->HandleFocusLost(LostHit);
	}
}

bool UFocusHostComponent::HandleInteract()
{
	if (!bHasFocusedHit)
	{
		return false;
	}

	const FRegistration* Registration = FindRegistration(FocusedRegistration);
	UFocusHandler* Handler = Registration ? Registration->Handler.Get() : nullptr;
	if (!Registration || !Registration->Owner.IsValid() || !IsValid(Handler))
	{
		ClearFocus();
		return false;
	}

	return Handler->HandleInteract(FocusedHit);
}

bool UFocusHostComponent::HandlePrimaryUse()
{
	if (!bHasFocusedHit)
	{
		return false;
	}

	const FRegistration* Registration = FindRegistration(FocusedRegistration);
	UFocusHandler* Handler = Registration ? Registration->Handler.Get() : nullptr;
	if (!Registration || !Registration->Owner.IsValid() || !IsValid(Handler))
	{
		ClearFocus();
		return false;
	}

	return Handler->HandlePrimaryUse(FocusedHit);
}

bool UFocusHostComponent::TryResolveFocusedPrompt(
	FFocusInteractionPrompt& OutPrompt) const
{
	OutPrompt = {};
	if (!bHasFocusedHit)
	{
		return false;
	}

	const FRegistration* Registration = FindRegistration(FocusedRegistration);
	const UFocusHandler* Handler = Registration ? Registration->Handler.Get() : nullptr;
	return Registration
		&& Registration->Owner.IsValid()
		&& IsValid(Handler)
		&& Handler->TryResolvePrompt(FocusedHit, OutPrompt);
}

const FFocusQueryHit* UFocusHostComponent::GetFocusedHit() const
{
	return bHasFocusedHit ? &FocusedHit : nullptr;
}

void UFocusHostComponent::BeginPlay()
{
	Super::BeginPlay();

	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	const bool bCanRunLocally = PlayerController
		&& PlayerController->IsLocalController()
		&& GetNetMode() != NM_DedicatedServer;
	SetComponentTickEnabled(bCanRunLocally);
	if (!bCanRunLocally)
	{
		ClearFocus();
	}
}

void UFocusHostComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearFocus();
	Registrations.Reset();
	QueryHitScratch.Reset();
	Super::EndPlay(EndPlayReason);
}

void UFocusHostComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	PruneInvalidRegistrations();
	if (Registrations.IsEmpty())
	{
		ClearFocus();
		return;
	}

	FFocusQueryContext Context;
	if (BuildViewContext(Context))
	{
		EvaluateFocus(Context);
	}
	else
	{
		ClearFocus();
	}
}

bool UFocusHostComponent::BuildViewContext(FFocusQueryContext& OutContext) const
{
	const APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return false;
	}

	FRotator ViewRotation = FRotator::ZeroRotator;
	PlayerController->GetPlayerViewPoint(OutContext.ViewOrigin, ViewRotation);
	OutContext.ViewDirection = ViewRotation.Vector().GetSafeNormal();
	return OutContext.IsValid();
}

const UFocusHostComponent::FRegistration* UFocusHostComponent::FindRegistration(
	const FFocusQueryRegistrationHandle Handle) const
{
	return Registrations.FindByPredicate(
		[Handle](const FRegistration& Registration)
		{
			return Registration.Handle == Handle;
		});
}

void UFocusHostComponent::ApplyFocus(FCandidate&& Candidate)
{
	const FRegistration* Registration = FindRegistration(Candidate.Registration);
	UFocusHandler* Handler = Registration ? Registration->Handler.Get() : nullptr;
	if (!Registration || !Registration->Owner.IsValid() || !IsValid(Handler))
	{
		ClearFocus();
		return;
	}

	if (bHasFocusedHit && FocusedRegistration == Candidate.Registration)
	{
		const FRegistration* CurrentRegistration = FindRegistration(FocusedRegistration);
		UFocusHandler* CurrentHandler = CurrentRegistration
			? CurrentRegistration->Handler.Get()
			: nullptr;
		if (CurrentHandler == Handler
			&& Handler->IsSameTarget(FocusedHit, Candidate.Hit))
		{
			FocusedHit = MoveTemp(Candidate.Hit);
			return;
		}
	}

	ClearFocus();
	FocusedRegistration = Candidate.Registration;
	FocusedHit = MoveTemp(Candidate.Hit);
	bHasFocusedHit = true;
	TGuardValue<bool> DispatchGuard(bDispatchingFocusChange, true);
	Handler->HandleFocusGained(FocusedHit);
}

void UFocusHostComponent::PruneInvalidRegistrations()
{
	if (bEvaluatingQueries)
	{
		return;
	}

	const auto IsInvalidRegistration = [](const FRegistration& Registration)
	{
		return !Registration.Owner.IsValid()
			|| !Registration.Handler.IsValid()
			|| !Registration.Query.IsBound();
	};

	const FRegistration* CurrentRegistration = FindRegistration(FocusedRegistration);
	if (CurrentRegistration && IsInvalidRegistration(*CurrentRegistration))
	{
		ClearFocus();
	}

	Registrations.RemoveAll(
		[&IsInvalidRegistration](const FRegistration& Registration)
		{
			return IsInvalidRegistration(Registration);
		});
}
