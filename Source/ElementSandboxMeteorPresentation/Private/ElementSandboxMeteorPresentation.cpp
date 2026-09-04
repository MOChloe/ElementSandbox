#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogElementSandboxMeteorPresentationModule, Log, All);

class FElementSandboxMeteorPresentationModule final : public IModuleInterface
{
public:
};

IMPLEMENT_MODULE(FElementSandboxMeteorPresentationModule, ElementSandboxMeteorPresentation)
