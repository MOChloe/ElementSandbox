#include "Elements/Fire/FireballProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "BuildingWorldSubsystem.h"
#include "ElementGameplayWorldSubsystem.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
constexpr float FireballGravityScale = 0.45f;
constexpr float MaximumAuthoritySubstepSeconds = 1.0f / 60.0f;
constexpr float FireballCollisionSourcePaddingCentimeters = 2.0f;
constexpr float FireballCollisionPrefetchSeconds = 0.25f;
constexpr float FireballCollisionRetentionPaddingCentimeters = 100.0f;
}

AFireballProjectile::AFireballProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bReplicates = true;
	// 只复制发射初态与权威命中；两端飞行均按各自帧率积分。
	SetReplicateMovement(false);
	SetNetUpdateFrequency(8.0f);

	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	SetRootComponent(CollisionSphere);
	CollisionSphere->InitSphereRadius(18.0f);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	CollisionSphere->SetGenerateOverlapEvents(false);
	CollisionSphere->SetCanEverAffectNavigation(false);
	CollisionSphere->SetCastShadow(false);

}

void AFireballProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AFireballProjectile, LaunchState);
	DOREPLIFETIME(AFireballProjectile, ImpactLocation);
	DOREPLIFETIME(AFireballProjectile, ImpactNormal);
	DOREPLIFETIME(AFireballProjectile, bImpacted);
}

void AFireballProjectile::BeginPlay()
{
	Super::BeginPlay();
	EnsureVisualComponents();
	if (AActor* InstigatorActor = GetInstigator())
	{
		CollisionSphere->IgnoreActorWhenMoving(InstigatorActor, true);
	}
	if (GetNetMode() == NM_Client)
	{
		// 客户端飞行是表现预测；碰撞与 Element 后果始终由 Authority 决定。
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SetActorTickEnabled(true);
	}
	else
	{
		SetActorTickEnabled(true);
	}
}

void AFireballProjectile::EnsureVisualComponents()
{
	if (GetNetMode() == NM_DedicatedServer || FlightBall || LowerImpactFlame
		|| UpperImpactFlame)
	{
		return;
	}
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UMaterialInterface* FlameMaterial = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Building/Materials/MI_FirePileFlame.MI_FirePileFlame"));
	const auto CreateMesh = [this, FlameMaterial](const FName Name)
	{
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, Name);
		AddInstanceComponent(Mesh);
		Mesh->SetupAttachment(CollisionSphere);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->bCastDynamicShadow = false;
		Mesh->bCastStaticShadow = false;
		if (FlameMaterial) Mesh->SetMaterial(0, FlameMaterial);
		Mesh->RegisterComponent();
		return Mesh;
	};
	FlightBall = CreateMesh(TEXT("FlightBall"));
	FlightBall->SetStaticMesh(SphereMesh);
	FlightBall->SetRelativeScale3D(FVector(0.34));
	LowerImpactFlame = CreateMesh(TEXT("LowerImpactFlame"));
	LowerImpactFlame->SetStaticMesh(ConeMesh);
	LowerImpactFlame->SetRelativeLocation(FVector(0.0, 0.0, 55.0));
	LowerImpactFlame->SetRelativeScale3D(FVector(1.05, 1.05, 1.25));
	UpperImpactFlame = CreateMesh(TEXT("UpperImpactFlame"));
	UpperImpactFlame->SetStaticMesh(ConeMesh);
	UpperImpactFlame->SetRelativeLocation(FVector(0.0, 0.0, 125.0));
	UpperImpactFlame->SetRelativeScale3D(FVector(0.62, 0.62, 0.90));
	UpdateVisualState();
}

void AFireballProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ExpireTimerHandle);
	ReleaseBuildingCollisionSource();
	ReleaseImpactFire();
	Super::EndPlay(EndPlayReason);
}

void AFireballProjectile::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (GetNetMode() == NM_Client && bVisualInitialized && !bImpacted)
	{
		AdvanceClientPrediction(DeltaSeconds);
	}
	else if (HasAuthority() && GetNetMode() != NM_Client && bVisualInitialized && !bImpacted)
	{
		AdvanceAuthorityFlight(DeltaSeconds);
	}
}

bool AFireballProjectile::LaunchAuthority(const FVector& Direction, const FPredictionKey& PredictionKey)
{
	if (!HasAuthority() || GetNetMode() == NM_Client || bImpacted ||
		Direction.ContainsNaN())
	{
		return false;
	}
	const FVector NormalizedDirection = Direction.GetSafeNormal();
	if (NormalizedDirection.IsNearlyZero())
	{
		return false;
	}
	LaunchState.Location = GetActorLocation();
	LaunchState.Velocity = NormalizedDirection * LaunchSpeedCentimetersPerSecond;
	LaunchState.ServerTimeSeconds = GetSynchronizedServerTimeSeconds();
	LaunchState.PredictionKey = PredictionKey;
	bVisualInitialized = true;
	SetActorTickEnabled(true);
	SetActorRotation(NormalizedDirection.Rotation());
	InitializeFlightState(LaunchState.Velocity);
	const FVector InitialEnd = GetActorLocation()
		+ FlightVelocity * MaximumAuthoritySubstepSeconds
		+ GetFlightGravity()
			* (0.5f * MaximumAuthoritySubstepSeconds * MaximumAuthoritySubstepSeconds);
	if (!RefreshBuildingCollisionSource(
			GetActorLocation(), InitialEnd, FlightVelocity))
	{
		bVisualInitialized = false;
		FlightVelocity = FVector::ZeroVector;
		return false;
	}
	SetLifeSpan(MaximumFlightSeconds);
	UpdateVisualState();
	ForceNetUpdate();
	return true;
}

bool AFireballProjectile::LaunchLocalPrediction(const FVector& Direction, const FPredictionKey& PredictionKey)
{
	// 客户端本地 Spawn 的非复制 Actor 也具有本地 ROLE_Authority；这里必须按
	// World NetMode 区分，不能把它误认成 Gameplay Authority。
	if (GetNetMode() != NM_Client || bImpacted || !PredictionKey.IsLocalClientKey() || Direction.ContainsNaN())
	{
		return false;
	}
	const FVector NormalizedDirection = Direction.GetSafeNormal();
	if (NormalizedDirection.IsNearlyZero())
	{
		return false;
	}

	bLocalPredictionProxy = true;
	SetReplicates(false);
	SetReplicateMovement(false);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LaunchState.Location = GetActorLocation();
	LaunchState.Velocity = NormalizedDirection * LaunchSpeedCentimetersPerSecond;
	LaunchState.ServerTimeSeconds = GetSynchronizedServerTimeSeconds();
	LaunchState.PredictionKey = PredictionKey;
	bVisualInitialized = true;
	SetActorRotation(NormalizedDirection.Rotation());
	InitializeFlightState(LaunchState.Velocity);
	SetActorTickEnabled(true);
	SetLifeSpan(MaximumFlightSeconds);
	UpdateVisualState();
	return true;
}

void AFireballProjectile::RejectLocalPrediction()
{
	if (bLocalPredictionProxy && GetNetMode() == NM_Client)
	{
		Destroy();
	}
}

bool AFireballProjectile::ImpactAtLocation(const FVector& InImpactLocation, const FVector& InImpactNormal)
{
	if (!HasAuthority() || bImpacted || InImpactLocation.ContainsNaN() || InImpactNormal.ContainsNaN())
	{
		return false;
	}
	const FVector SafeNormal =
		InImpactNormal.GetSafeNormal().IsNearlyZero() ? FVector::UpVector : InImpactNormal.GetSafeNormal();
	ImpactLocation = InImpactLocation + SafeNormal * 5.0;
	ImpactNormal = SafeNormal;
	bImpacted = true;
	FlightVelocity = FVector::ZeroVector;
	ReleaseBuildingCollisionSource();
	SetActorTickEnabled(false);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorLocationAndRotation(ImpactLocation, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
	SetLifeSpan(0.0f);
	UpdateVisualState();

	UElementGameplayWorldSubsystem* Fire =
		GetWorld() ? GetWorld()->GetSubsystem<UElementGameplayWorldSubsystem>() : nullptr;
	ImpactFireSource = Fire ? Fire->CreateFireballSource(ImpactLocation)
		: FElementRuntimeFireSourceHandle();
	if (!ImpactFireSource.IsSet())
	{
		Destroy();
		return false;
	}
	const int64 ImpactLifetimeMilliseconds = Fire->GetFireballSourceLifetimeMilliseconds();
	if (ImpactLifetimeMilliseconds <= 0)
	{
		ReleaseImpactFire();
		Destroy();
		return false;
	}

	GetWorldTimerManager().SetTimer(ExpireTimerHandle,
		this,
		&AFireballProjectile::ExpireAndDestroy,
		static_cast<float>(ImpactLifetimeMilliseconds) / 1000.0f,
		false);
	ForceNetUpdate();
	return true;
}

void AFireballProjectile::OnRep_Impacted()
{
	if (bImpacted)
	{
		ApplyAuthoritativeImpact();
	}
}

void AFireballProjectile::OnRep_LaunchState()
{
	if (bImpacted)
	{
		ApplyAuthoritativeImpact();
		return;
	}
	StartClientFlightPrediction();
}

void AFireballProjectile::UpdateVisualState()
{
	const bool bShowFlight = bVisualInitialized && !bImpacted;
	const bool bShowImpact = bVisualInitialized && bImpacted;
	if (IsValid(FlightBall))
	{
		FlightBall->SetHiddenInGame(!bShowFlight, true);
		FlightBall->SetVisibility(bShowFlight, true);
	}
	for (UStaticMeshComponent* Flame : {LowerImpactFlame.Get(), UpperImpactFlame.Get()})
	{
		if (IsValid(Flame))
		{
			Flame->SetHiddenInGame(!bShowImpact, true);
			Flame->SetVisibility(bShowImpact, true);
		}
	}
}

void AFireballProjectile::StartClientFlightPrediction()
{
	if (HasAuthority() || bLocalPredictionProxy || LaunchState.Velocity.IsNearlyZero())
	{
		return;
	}

	FVector PredictedLocation = LaunchState.Location;
	FVector PredictedVelocity = LaunchState.Velocity;
	float PredictionAge = 0.0f;
	if (AFireballProjectile* LocalPrediction = FindMatchingLocalPrediction())
	{
		PredictedLocation = LocalPrediction->GetActorLocation();
		PredictedVelocity = LocalPrediction->FlightVelocity;
		PredictionAge = LocalPrediction->SimulatedFlightSeconds;
		LocalPrediction->Destroy();
	}
	else
	{
		PredictionAge = FMath::Clamp(
			GetSynchronizedServerTimeSeconds() - LaunchState.ServerTimeSeconds, 0.0f, MaximumFlightSeconds);
		const FVector Gravity = GetFlightGravity();
		PredictedLocation += LaunchState.Velocity * PredictionAge + Gravity * (0.5f * PredictionAge * PredictionAge);
		PredictedVelocity += Gravity * PredictionAge;
	}

	bVisualInitialized = true;
	SetActorLocationAndRotation(
		PredictedLocation, PredictedVelocity.Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
	InitializeFlightState(PredictedVelocity, PredictionAge);
	SetActorTickEnabled(true);
	SetLifeSpan(MaximumFlightSeconds);
	UpdateVisualState();
}

void AFireballProjectile::ApplyAuthoritativeImpact()
{
	if (HasAuthority() || bLocalPredictionProxy || !bImpacted)
	{
		return;
	}
	if (AFireballProjectile* LocalPrediction = FindMatchingLocalPrediction())
	{
		LocalPrediction->Destroy();
	}
	FlightVelocity = FVector::ZeroVector;
	SetActorTickEnabled(false);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorLocationAndRotation(ImpactLocation, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
	bVisualInitialized = true;
	UpdateVisualState();
}

void AFireballProjectile::AdvanceClientPrediction(const float DeltaSeconds)
{
	const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (SafeDeltaSeconds <= UE_KINDA_SMALL_NUMBER || FlightVelocity.IsNearlyZero())
	{
		return;
	}
	const FVector Gravity = GetFlightGravity();
	const FVector MoveDelta =
		FlightVelocity * SafeDeltaSeconds + Gravity * (0.5f * SafeDeltaSeconds * SafeDeltaSeconds);
	FlightVelocity += Gravity * SafeDeltaSeconds;
	SimulatedFlightSeconds += SafeDeltaSeconds;
	SetActorLocationAndRotation(GetActorLocation() + MoveDelta,
		FlightVelocity.Rotation(),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
}

bool AFireballProjectile::AdvanceAuthorityFlight(const float DeltaSeconds)
{
	float RemainingSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.5f);
	const FVector Gravity = GetFlightGravity();
	while (RemainingSeconds > UE_KINDA_SMALL_NUMBER && !bImpacted)
	{
		const float StepSeconds = FMath::Min(RemainingSeconds, MaximumAuthoritySubstepSeconds);
		const FVector MoveDelta = FlightVelocity * StepSeconds + Gravity * (0.5f * StepSeconds * StepSeconds);
		const FVector NextVelocity = FlightVelocity + Gravity * StepSeconds;
		const FVector SegmentStart = GetActorLocation();
		const FVector SegmentEnd = SegmentStart + MoveDelta;
		if (!RefreshBuildingCollisionSource(SegmentStart, SegmentEnd, NextVelocity))
		{
			UE_LOG(LogTemp, Error,
				TEXT("Fireball 无法刷新 Building Collision Source；为避免穿透，终止投射物。"));
			Destroy();
			return false;
		}
		FHitResult Hit;
		CollisionSphere->MoveComponent(MoveDelta,
			NextVelocity.Rotation().Quaternion(),
			true,
			&Hit,
			MOVECOMP_NoFlags,
			ETeleportType::None);
		FlightVelocity = NextVelocity;
		SimulatedFlightSeconds += StepSeconds;
		RemainingSeconds -= StepSeconds;
		if (Hit.IsValidBlockingHit())
		{
			return ImpactAtLocation(Hit.ImpactPoint, Hit.ImpactNormal);
		}
	}
	return !bImpacted;
}

bool AFireballProjectile::RefreshBuildingCollisionSource(
	const FVector& SegmentStart,
	const FVector& SegmentEnd,
	const FVector& Velocity)
{
	if (!HasAuthority() || GetNetMode() == NM_Client
		|| SegmentStart.ContainsNaN() || SegmentEnd.ContainsNaN()
		|| Velocity.ContainsNaN())
	{
		return false;
	}
	UWorld* World = GetWorld();
	UBuildingWorldSubsystem* Buildings = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!Buildings)
	{
		return false;
	}

	const double Radius = CollisionSphere
		? CollisionSphere->GetScaledSphereRadius()
		: 18.0;
	const FVector Extent(Radius + FireballCollisionSourcePaddingCentimeters);
	FBuildCollisionSource Source;
	Source.SubjectLocation = SegmentStart;
	Source.Velocity = Velocity;
	Source.ImmediateBounds = FBox(SegmentStart - Extent, SegmentStart + Extent);
	Source.ImmediateBounds += FBox(SegmentEnd - Extent, SegmentEnd + Extent);
	Source.CameraBounds = Source.ImmediateBounds;
	Source.PrefetchBounds = Source.ImmediateBounds;
	const FVector PrefetchEnd = SegmentEnd
		+ Velocity * FireballCollisionPrefetchSeconds;
	Source.PrefetchBounds += FBox(PrefetchEnd - Extent, PrefetchEnd + Extent);
	Source.RetentionBounds = Source.PrefetchBounds.ExpandBy(
		FireballCollisionRetentionPaddingCentimeters);
	Source.Revision = BuildingCollisionSourceRevision++;
	if (BuildingCollisionSourceRevision == 0)
	{
		++BuildingCollisionSourceRevision;
	}

	bool bSubmitted = BuildingCollisionSource.IsSet()
		&& Buildings->UpdateCollisionSource(BuildingCollisionSource, Source);
	if (!bSubmitted)
	{
		BuildingCollisionSource = Buildings->RegisterCollisionSource(Source);
		bSubmitted = BuildingCollisionSource.IsSet();
	}
	// 当前子步 Sweep 前必须同步补齐 Immediate Body；Prefetch 仍受 Building
	// Processor 自身预算约束，不能让高速 Fireball 跑在局部碰撞前面。
	return bSubmitted && Buildings->FlushCollisionChanges();
}

void AFireballProjectile::ReleaseBuildingCollisionSource()
{
	if (!BuildingCollisionSource.IsSet())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		if (UBuildingWorldSubsystem* Buildings =
			World->GetSubsystem<UBuildingWorldSubsystem>())
		{
			Buildings->UnregisterCollisionSource(BuildingCollisionSource);
		}
	}
	BuildingCollisionSource = {};
}

void AFireballProjectile::InitializeFlightState(const FVector& Velocity, const float ElapsedSeconds)
{
	FlightVelocity = Velocity;
	SimulatedFlightSeconds = FMath::Max(0.0f, ElapsedSeconds);
}

FVector AFireballProjectile::GetFlightGravity() const
{
	const UWorld* World = GetWorld();
	return FVector(0.0, 0.0, (World ? World->GetGravityZ() : -980.0f) * FireballGravityScale);
}

AFireballProjectile* AFireballProjectile::FindMatchingLocalPrediction() const
{
	if (!GetWorld() || !LaunchState.PredictionKey.IsValidKey())
	{
		return nullptr;
	}
	for (TActorIterator<AFireballProjectile> It(GetWorld()); It; ++It)
	{
		AFireballProjectile* Candidate = *It;
		if (Candidate != this && Candidate->bLocalPredictionProxy &&
			Candidate->LaunchState.PredictionKey == LaunchState.PredictionKey)
		{
			return Candidate;
		}
	}
	return nullptr;
}

float AFireballProjectile::GetSynchronizedServerTimeSeconds() const
{
	const UWorld* World = GetWorld();
	const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	return GameState ? GameState->GetServerWorldTimeSeconds() : World ? World->GetTimeSeconds() : 0.0f;
}

void AFireballProjectile::ExpireAndDestroy()
{
	ReleaseImpactFire();
	Destroy();
}

void AFireballProjectile::ReleaseImpactFire()
{
	if (bReleasingImpactFire || !ImpactFireSource.IsSet())
	{
		return;
	}
	bReleasingImpactFire = true;
	UElementGameplayWorldSubsystem* Fire =
		GetWorld() ? GetWorld()->GetSubsystem<UElementGameplayWorldSubsystem>() : nullptr;
	if (!Fire || Fire->RemoveRuntimeFireSource(ImpactFireSource))
	{
		ImpactFireSource = {};
	}
	bReleasingImpactFire = false;
}
