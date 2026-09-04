#include "Processing/BuildProcessor.h"

#include "Processing/BuildProcessorScheduler.h"

FBuildProcessor::FBuildProcessor(const FName InDebugName)
	: DebugName(InDebugName)
{
	check(!DebugName.IsNone());
}

FBuildProcessor::~FBuildProcessor()
{
	checkf(!Scheduler, TEXT("Build Processor must be detached before destruction."));
}

bool FBuildProcessor::RequestExecution()
{
	check(IsInGameThread());
	return Scheduler && Scheduler->RequestExecution(*this);
}

bool FBuildProcessor::IsRegistered() const
{
	check(IsInGameThread());
	return Scheduler && Registration.IsSet();
}

void FBuildProcessor::Attach(
	FBuildProcessorScheduler& InScheduler,
	const FBuildProcessorRegistrationHandle InRegistration)
{
	check(IsInGameThread());
	check(!Scheduler && !Registration.IsSet() && InRegistration.IsSet());
	Scheduler = &InScheduler;
	Registration = InRegistration;
}

void FBuildProcessor::Detach()
{
	check(IsInGameThread());
	Scheduler = nullptr;
	Registration = {};
}
