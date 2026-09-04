#include "WorldStorageProcessRole.h"

#include "CoreGlobals.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

bool FWorldStorageProcessRole::ShouldUseExternalLocalServer(const FWorldStorageProcessRoleContext& Context)
{
	return (Context.bGameProcess || Context.bPlayInEditorStandaloneWorld)
		&& !Context.bCommandlet
		&& !Context.bAutomationTesting
		&& !Context.bDedicatedServer
		&& !Context.bLocalServerChild
		&& !Context.bLocalServerLaunchSuppressed;
}

bool FWorldStorageProcessRole::ShouldUseExternalLocalServerForCurrentWorld(const UWorld* World)
{
	FWorldStorageProcessRoleContext Context;
	Context.bGameProcess = FApp::IsGame();
	Context.bPlayInEditorStandaloneWorld = World
		&& World->WorldType == EWorldType::PIE
		&& World->GetNetMode() == NM_Standalone;
	Context.bCommandlet = IsRunningCommandlet();
	Context.bAutomationTesting = GIsAutomationTesting;
	Context.bDedicatedServer = IsRunningDedicatedServer()
		|| (World && World->GetNetMode() == NM_DedicatedServer);
	Context.bLocalServerChild = FParse::Param(
		FCommandLine::Get(), TEXT("ElementSandboxLocalServerChild"));
	Context.bLocalServerLaunchSuppressed = FParse::Param(
		FCommandLine::Get(), TEXT("ElementSandboxNoLocalServer"));
	return ShouldUseExternalLocalServer(Context);
}
