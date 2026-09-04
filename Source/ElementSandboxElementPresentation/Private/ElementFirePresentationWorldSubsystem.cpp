#include "ElementFirePresentationWorldSubsystem.h"

#include "ElementPresentationWorldSubsystem.h"
#include "ElementSimulationSubsystem.h"
#include "ElementVisualDefinition.h"
#include "Visual/ElementVisualJournal.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	const FName FireVisualDefinitionId(TEXT("Element.Fire.Flame"));
}

bool UElementFirePresentationWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World
		&& World->GetNetMode() != NM_DedicatedServer
		&& World->GetGameInstance() != nullptr;
}

void UElementFirePresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UElementSimulationSubsystem>();
	Collection.InitializeDependency<UElementPresentationWorldSubsystem>();
	UWorld* World = GetWorld();
	UElementSimulationSubsystem* Simulation = World
		? World->GetSubsystem<UElementSimulationSubsystem>() : nullptr;
	UElementPresentationWorldSubsystem* Presentation = World
		? World->GetSubsystem<UElementPresentationWorldSubsystem>() : nullptr;
	if (!Simulation || !Presentation || !Simulation->GetVisualJournal()) return;
	if (!Presentation->IsConfigured()
		&& !Presentation->Configure(FElementPresentationConfig())) return;

	FlameMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	FlameMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	FElementVisualDefinition Definition;
	Definition.DefinitionId = FireVisualDefinitionId;
	Definition.StaticMesh = FlameMesh;
	Definition.MaterialOverride = FlameMaterial;
	Definition.Backend = EElementVisualInstanceBackend::Instanced;
	Definition.CustomDataFloatCount = 0;
	if (!Presentation->RegisterVisualDefinition(Definition)) return;
	VisualSourceHandle = Presentation->RegisterVisualSource(
		StaticCastSharedRef<IElementVisualSource>(
			Simulation->GetVisualJournal().ToSharedRef()));
	if (!VisualSourceHandle.IsSet())
	{
		Presentation->UnregisterVisualDefinition(FireVisualDefinitionId);
	}
}

void UElementFirePresentationWorldSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UElementPresentationWorldSubsystem* Presentation =
			World->GetSubsystem<UElementPresentationWorldSubsystem>())
		{
			if (VisualSourceHandle.IsSet())
				Presentation->UnregisterVisualSource(VisualSourceHandle);
			Presentation->UnregisterVisualDefinition(FireVisualDefinitionId);
		}
	}
	VisualSourceHandle = {};
	FlameMesh = nullptr;
	FlameMaterial = nullptr;
	Super::Deinitialize();
}

bool UElementFirePresentationWorldSubsystem::DoesSupportWorldType(
	const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
