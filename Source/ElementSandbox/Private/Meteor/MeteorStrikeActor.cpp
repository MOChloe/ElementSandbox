#include "Meteor/MeteorStrikeActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

bool FMeteorStrikeVisualState::IsValid() const
{
	return !StartLocation.ContainsNaN() && !ImpactLocation.ContainsNaN()
		&& FMath::IsFinite(StartServerTimeSeconds) && FMath::IsFinite(ImpactServerTimeSeconds)
		&& ImpactServerTimeSeconds > StartServerTimeSeconds
		&& FMath::IsFinite(MeteorDiameterCentimeters) && MeteorDiameterCentimeters > 0.0f;
}

AMeteorStrikeActor::AMeteorStrikeActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(2.0f);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	MeteorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeteorMesh"));
	MeteorMesh->SetupAttachment(SceneRoot);
	MeteorMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeteorMesh->SetGenerateOverlapEvents(false);
	MeteorMesh->SetCanEverAffectNavigation(false);
	MeteorMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Flame(
		TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	if (Sphere.Succeeded()) MeteorMesh->SetStaticMesh(Sphere.Object);
	if (Flame.Succeeded()) MeteorMesh->SetMaterial(0, Flame.Object);
}

void AMeteorStrikeActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMeteorStrikeActor, VisualState);
}

bool AMeteorStrikeActor::LaunchAuthority(
	const FVector& StartLocation,
	const FVector& ImpactLocation,
	const double StartTimeSeconds,
	const double ImpactTimeSeconds,
	const float MeteorDiameterCentimeters)
{
	if (!HasAuthority()) return false;
	VisualState.StartLocation = StartLocation;
	VisualState.ImpactLocation = ImpactLocation;
	VisualState.StartServerTimeSeconds = static_cast<float>(StartTimeSeconds);
	VisualState.ImpactServerTimeSeconds = static_cast<float>(ImpactTimeSeconds);
	VisualState.MeteorDiameterCentimeters = MeteorDiameterCentimeters;
	if (!VisualState.IsValid()) return false;
	ApplyVisualScale();
	SetActorTickEnabled(true);
	ApplyAbsoluteTime();
	SetLifeSpan(static_cast<float>(ImpactTimeSeconds - StartTimeSeconds + 2.0));
	ForceNetUpdate();
	return true;
}

void AMeteorStrikeActor::OnRep_VisualState()
{
	SetActorTickEnabled(VisualState.IsValid());
	ApplyVisualScale();
	ApplyAbsoluteTime();
}

void AMeteorStrikeActor::ApplyVisualScale()
{
	if (!VisualState.IsValid() || !MeteorMesh) return;
	constexpr float EngineBasicSphereDiameterCentimeters = 100.0f;
	MeteorMesh->SetRelativeScale3D(FVector(
		VisualState.MeteorDiameterCentimeters / EngineBasicSphereDiameterCentimeters));
	MeteorMesh->SetVisibility(true, true);
}

void AMeteorStrikeActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyAbsoluteTime();
}

void AMeteorStrikeActor::ApplyAbsoluteTime()
{
	if (!VisualState.IsValid()) return;
	const float Duration = VisualState.ImpactServerTimeSeconds - VisualState.StartServerTimeSeconds;
	const float Alpha = FMath::Clamp(
		(GetServerTimeSeconds() - VisualState.StartServerTimeSeconds) / Duration, 0.0f, 1.0f);
	// 二次 Ease-In 表现重力加速；权威 Gameplay 只依赖绝对 ImpactTime，不依赖此 Actor 的 Transform。
	const float AcceleratedAlpha = Alpha * Alpha;
	SetActorLocation(FMath::Lerp(FVector(VisualState.StartLocation),
		FVector(VisualState.ImpactLocation), AcceleratedAlpha), false, nullptr, ETeleportType::TeleportPhysics);
	AddActorLocalRotation(FRotator(0.0f, 90.0f * GetWorld()->GetDeltaSeconds(), 45.0f * GetWorld()->GetDeltaSeconds()));
	if (Alpha >= 1.0f)
	{
		MeteorMesh->SetVisibility(false, true);
		SetActorTickEnabled(false);
	}
}

float AMeteorStrikeActor::GetServerTimeSeconds() const
{
	const AGameStateBase* GameState = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	return GameState ? GameState->GetServerWorldTimeSeconds()
		: GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}
