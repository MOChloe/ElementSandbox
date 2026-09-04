#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"

DECLARE_LOG_CATEGORY_EXTERN(LogElementSandboxPresentation, Log, All);
DECLARE_STATS_GROUP(TEXT("ElementSandboxPresentation"), STATGROUP_ElementSandboxPresentation, STATCAT_Advanced);
CSV_DECLARE_CATEGORY_EXTERN(ElementSandboxPresentation);

class FElementSandboxPresentationModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
