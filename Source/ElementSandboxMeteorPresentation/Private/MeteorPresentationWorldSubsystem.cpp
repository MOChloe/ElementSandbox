#include "MeteorPresentationWorldSubsystem.h"
#include "MeteorClientWorldSubsystem.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "Engine/World.h"

using namespace UE::ElementSandbox::Meteor;
void UMeteorPresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UMeteorClientWorldSubsystem>();
	Collection.InitializeDependency<UWoodProductPresentationWorldSubsystem>();
	auto* Client = GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>();
	ChangesHandle = Client->OnPresentationChanges().AddUObject(this, &ThisClass::HandleChanges);
	RetiredHandle = Client->OnBurstRetired().AddUObject(this, &ThisClass::HandleRetired);
}
void UMeteorPresentationWorldSubsystem::Deinitialize()
{
	if (auto* Client = GetWorld()->GetSubsystem<UMeteorClientWorldSubsystem>())
	{
		Client->OnPresentationChanges().Remove(ChangesHandle);
		Client->OnBurstRetired().Remove(RetiredHandle);
	}
	Super::Deinitialize();
}
void UMeteorPresentationWorldSubsystem::HandleChanges(TConstArrayView<FMeteorClientPresentationLane> Changes)
{
	TArray<FWoodProductFlight> Flights;
	Flights.Reserve(Changes.Num());
	for (const auto& Change : Changes)
	{
		auto& Flight = Flights.AddDefaulted_GetRef();
		Flight.WorldEntityId = Change.WorldEntityId; Flight.DefinitionId = Change.RenderArchetypeId;
		Flight.BurstId = Change.BurstId.Value; Flight.BatchId = Change.PageId; Flight.Revision = Change.Revision;
		Flight.Phase = Change.State == EMeteorClientDebrisState::Prepared ? EWoodProductFlightPhase::Prepared
			: Change.State == EMeteorClientDebrisState::Flying ? EWoodProductFlightPhase::Active
			: Change.State == EMeteorClientDebrisState::Canceled ? EWoodProductFlightPhase::Canceled : EWoodProductFlightPhase::Settled;
		Flight.RestTransform = Change.RestTransform;
		Flight.StartOffset = FVector3f(Change.StartPosition - Change.RestTransform.GetLocation());
		Flight.ImpactOffset = FVector3f(Change.ImpactEndpoint - Change.RestTransform.GetLocation());
		Flight.Velocity = Change.InitialVelocity; Flight.Acceleration = Change.Acceleration;
		Flight.AngularVelocityDegrees = Change.AngularVelocityDegrees; Flight.StartRotation = Change.StartRotation;
		Flight.LocalStartTime = Change.LocalStartTimeSeconds; Flight.ImpactSeconds = Change.ImpactDurationSeconds;
		Flight.SettlingSeconds = Change.SettlingDurationSeconds; Flight.LiftHeight = Change.SettlingLiftHeight; Flight.Radius = Change.VisualRadius;
	}
	GetWorld()->GetSubsystem<UWoodProductPresentationWorldSubsystem>()->QueueFlightChanges(Flights);
}
void UMeteorPresentationWorldSubsystem::HandleRetired(FMeteorBurstId Burst)
{
	GetWorld()->GetSubsystem<UWoodProductPresentationWorldSubsystem>()->RetireFlightBurst(Burst.Value);
}
bool UMeteorPresentationWorldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const { return Type == EWorldType::Game || Type == EWorldType::PIE; }
