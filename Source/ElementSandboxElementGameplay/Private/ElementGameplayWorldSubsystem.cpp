#include "ElementGameplayWorldSubsystem.h"

#include "BuildingWorldSubsystem.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "ElementSimulationSubsystem.h"
#include "Fire/ElementFireRuleSet.h"
#include "Runtime/ElementFireDomain.h"
#include "Subsystems/SubsystemCollection.h"
#include "Tree/SettlementTreeWorldSubsystem.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

void FElementFireDomainDeleter::operator()(FElementFireDomain* Runtime) const
{
	delete Runtime;
}

UElementGameplayWorldSubsystem::UElementGameplayWorldSubsystem() = default;
UElementGameplayWorldSubsystem::~UElementGameplayWorldSubsystem() = default;

void UElementGameplayWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldStorageSubsystem>();
	Collection.InitializeDependency<UBuildingWorldSubsystem>();
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Collection.InitializeDependency<USettlementTreeWorldSubsystem>();
	Collection.InitializeDependency<UCharacterQuerySnapshotSubsystem>();
	Collection.InitializeDependency<UElementSimulationSubsystem>();
	Runtime.Reset(new FElementFireDomain(*this));
	bRuntimeAssemblyActive = Runtime && Runtime->Initialize();
	if (!bRuntimeAssemblyActive)
	{
		Runtime.Reset();
		UE_LOG(LogTemp, Error,
			TEXT("ElementGameplay Fire Runtime 装配失败；本 World 不启用不完整的 Fire Gameplay。"));
	}
}

void UElementGameplayWorldSubsystem::Deinitialize()
{
	if (Runtime) Runtime->Shutdown();
	Runtime.Reset();
	bRuntimeAssemblyActive = false;
	Super::Deinitialize();
}

FElementRuntimeFireSourceHandle UElementGameplayWorldSubsystem::CreateFireballSource(
	const FVector& WorldLocation)
{
	return Runtime ? Runtime->CreateFireballSource(WorldLocation)
		: FElementRuntimeFireSourceHandle();
}

int64 UElementGameplayWorldSubsystem::GetFireballSourceLifetimeMilliseconds() const
{
	return Runtime ? Runtime->GetRules().FireballLifetimeMilliseconds : 0;
}

bool UElementGameplayWorldSubsystem::RemoveRuntimeFireSource(
	const FElementRuntimeFireSourceHandle Handle)
{
	return Runtime && Runtime->RemoveRuntimeFireSource(Handle);
}

bool UElementGameplayWorldSubsystem::SetStickFireInteractionState(
	const FWorldEntityId WorldEntityId,
	const bool bActive)
{
	return Runtime && Runtime->SetStickFireInteractionState(WorldEntityId, bActive);
}

#if WITH_DEV_AUTOMATION_TESTS
bool UElementGameplayWorldSubsystem::IsStickFireInteractionActiveForTesting(
	const FWorldEntityId WorldEntityId) const
{
	return Runtime && Runtime->IsStickFireInteractionActiveForTesting(WorldEntityId);
}

int32 UElementGameplayWorldSubsystem::GetBuildingFireHostCountForTesting() const
{
	return Runtime ? Runtime->GetBuildingHostCountForTesting() : 0;
}
#endif

bool UElementGameplayWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
