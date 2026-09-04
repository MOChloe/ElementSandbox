#include "ElementSimulationSubsystem.h"

#include "Engine/World.h"
#include "Runtime/ElementAuthorityExecution.h"
#include "Visual/ElementVisualJournal.h"

UElementSimulationSubsystem::UElementSimulationSubsystem() = default;
UElementSimulationSubsystem::~UElementSimulationSubsystem() = default;

void UElementSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		VisualJournal = MakeShared<FElementVisualJournal, ESPMode::ThreadSafe>();
	}
}

void UElementSimulationSubsystem::Deinitialize()
{
	DeactivateAuthorityExecution();
	VisualJournal.Reset();
	Super::Deinitialize();
}

bool UElementSimulationSubsystem::ActivateAuthorityExecution(
	const FElementAuthorityExecutionConfig& Config)
{
	check(IsInGameThread());
	if (AuthorityExecution || !GetWorld() || GetWorld()->GetNetMode() == NM_Client
		|| !Config.IsValid())
	{
		return false;
	}
	AuthorityExecution = MakeUnique<FElementAuthorityExecution>(Config);
	return true;
}

void UElementSimulationSubsystem::DeactivateAuthorityExecution()
{
	check(IsInGameThread());
	AuthorityExecution.Reset();
}

bool UElementSimulationSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
