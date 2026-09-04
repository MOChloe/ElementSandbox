#include "Projection/WorldObjectPhysicsProxyActor.h"

#include "Components/BoxComponent.h"
#include "Collision/WorldObjectCollisionWorldSubsystem.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "Net/UnrealNetwork.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsReplication.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "TimerManager.h"
#include "WorldObjectWorldSubsystem.h"

namespace
{
	/** 即使近似 Box 仍有少量重叠，也只允许 Chaos 以低速解穿透，避免残骸弹射。 */
	constexpr float LooseDebrisMaxDepenetrationVelocity = 60.0f;
	/**
	 * 与 Server Authority 8 Hz 无关；这里只给初始 Actor/Live Delta 留出数个 30 Hz 网络帧，
	 * 让 Client 的 HISM 在刚体开始移动前先出现在生成位置。
	 */
	constexpr float InitialPhysicsReplicationLeadSeconds = 0.15f;
}

AWorldObjectPhysicsProxyActor::AWorldObjectPhysicsProxyActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = false;
	// Awake Physics 使用 Actor Movement 通道，但只投影给 15km Retention 范围内的连接。
	SetNetCullDistanceSquared(FMath::Square(1500000.0f));
	bNetLoadOnClient = false;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);

	PhysicsBox = CreateDefaultSubobject<UBoxComponent>(TEXT("PhysicsBox"));
	SetRootComponent(PhysicsBox);
	PhysicsBox->InitBoxExtent(FVector(1.0f));
	PhysicsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PhysicsBox->SetGenerateOverlapEvents(false);
	PhysicsBox->SetCanEverAffectNavigation(false);
	PhysicsBox->SetCastShadow(false);
	PhysicsBox->bCastDynamicShadow = false;
	PhysicsBox->bCastStaticShadow = false;
	PhysicsBox->CanCharacterStepUpOn = ECB_Yes;
	PhysicsBox->BodyInstance.bGenerateWakeEvents = true;
	PhysicsBox->BodyInstance.bUseCCD = true;
	PhysicsBox->SetLinearDamping(
		UE::ElementSandbox::WorldObjects::Physics::DefaultLinearDamping);
	PhysicsBox->SetAngularDamping(
		UE::ElementSandbox::WorldObjects::Physics::DefaultAngularDamping);

	PhysicsMaterial = CreateDefaultSubobject<UPhysicalMaterial>(TEXT("PhysicsMaterial"));
	PhysicsMaterial->Friction = UE::ElementSandbox::WorldObjects::Physics::DefaultFriction;
	PhysicsMaterial->Restitution =
		UE::ElementSandbox::WorldObjects::Physics::DefaultRestitution;

	WorldObjectProxyComponent = CreateDefaultSubobject<UWorldObjectProxyComponent>(
		TEXT("WorldObjectProxyComponent"));
}

void AWorldObjectPhysicsProxyActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWorldObjectPhysicsProxyActor, PhysicsConfiguration);
	DOREPLIFETIME(AWorldObjectPhysicsProxyActor, bPhysicsReleased);
}

void AWorldObjectPhysicsProxyActor::BeginPlay()
{
	Super::BeginPlay();
	if (PhysicsConfiguration.IsValid() && !PhysicsBox->IsSimulatingPhysics())
	{
		ApplyPhysicsConfiguration();
	}
}

void AWorldObjectPhysicsProxyActor::PostNetReceive()
{
	Super::PostNetReceive();
	RefreshClientPhysicsProjection();
}

void AWorldObjectPhysicsProxyActor::OnRep_ReplicatedMovement()
{
	if (!PhysicsConfiguration.IsValid() || (!HasAuthority() && !bClientPhysicsProjectionActive))
	{
		bHasDeferredReplicatedMovement = true;
		return;
	}
	bHasDeferredReplicatedMovement = false;
	// LooseDebris 保留完整刚体快照，交给 UE PredictiveInterpolation 保留本地接触并纠正。
	Super::OnRep_ReplicatedMovement();
	ApplyPhysicsReleaseState();
}

bool AWorldObjectPhysicsProxyActor::ConfigurePhysics(
	const FBox& LocalBounds,
	const float InMassKg,
	const EWorldObjectPhysicsCollisionPolicy CollisionPolicy,
	const FVector& LinearVelocity,
	const FVector& AngularVelocityDegrees,
	const uint32 ActivationRevision)
{
	if (!HasAuthority()
		|| ActivationRevision == 0 || LocalBounds.IsValid == 0
		|| LocalBounds.ContainsNaN()
		|| !FMath::IsFinite(InMassKg)
			|| InMassKg <= UE_SMALL_NUMBER
			|| LinearVelocity.ContainsNaN()
				|| AngularVelocityDegrees.ContainsNaN())
	{
		return false;
	}

	PhysicsConfiguration.bConfigured = true;
	PhysicsConfiguration.LocalCenter = LocalBounds.GetCenter();
	PhysicsConfiguration.LocalExtent = LocalBounds.GetExtent();
	PhysicsConfiguration.MassKg = InMassKg;
	PhysicsConfiguration.ActivationRevision = ActivationRevision;
	PhysicsConfiguration.CollisionPolicy = CollisionPolicy;
	if (!PhysicsConfiguration.IsValid())
	{
		PhysicsConfiguration = {};
		return false;
	}

	PendingInitialLinearVelocity = LinearVelocity;
	PendingInitialAngularVelocityDegrees = AngularVelocityDegrees;
	GetWorldTimerManager().ClearTimer(PhysicsReleaseTimer);
	bPhysicsReleased = false;
	ApplyPhysicsConfiguration();
	ForceNetUpdate();
	return true;
}

void AWorldObjectPhysicsProxyActor::SchedulePhysicsRelease()
{
	if (!HasAuthority() || bPhysicsReleased || !PhysicsConfiguration.IsValid()
		|| GetWorldTimerManager().IsTimerActive(PhysicsReleaseTimer))
	{
		return;
	}

	// 先强制发送配置态与初始 Transform；Timer 到期后 bPhysicsReleased=true
	// 会形成第二个明确的复制边界，而不是与 Actor 初始 Bunch 合并。
	ForceNetUpdate();
	GetWorldTimerManager().SetTimer(
		PhysicsReleaseTimer,
		this,
		&AWorldObjectPhysicsProxyActor::ReleasePhysicsImmediately,
		InitialPhysicsReplicationLeadSeconds,
		false);
}

FTransform AWorldObjectPhysicsProxyActor::GetWorldObjectTransform() const
{
	if (!PhysicsConfiguration.IsValid())
	{
		return GetActorTransform();
	}
	return FTransform(
		FQuat::Identity,
		-PhysicsConfiguration.LocalCenter) * GetActorTransform();
}

FTransform AWorldObjectPhysicsProxyActor::MakeActorTransform(
	const FTransform& WorldObjectTransform,
	const FBox& LocalBounds)
{
	return FTransform(FQuat::Identity, LocalBounds.GetCenter()) * WorldObjectTransform;
}

void AWorldObjectPhysicsProxyActor::OnRep_PhysicsConfiguration()
{
	ApplyPhysicsConfiguration();
	if (UWorldObjectWorldSubsystem* Objects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		Objects->RegisterProxy(*WorldObjectProxyComponent);
	}
	RefreshClientPhysicsProjection();
}

void AWorldObjectPhysicsProxyActor::OnRep_PhysicsReleased()
{
	ApplyPhysicsReleaseState();
}

void AWorldObjectPhysicsProxyActor::ApplyPhysicsConfiguration()
{
	if (!PhysicsConfiguration.IsValid() || !IsValid(PhysicsBox))
	{
		return;
	}
	const bool bLooseDebris =
		PhysicsConfiguration.CollisionPolicy == EWorldObjectPhysicsCollisionPolicy::LooseDebris;
	const EPhysicsReplicationMode ReplicationMode = bLooseDebris
		? EPhysicsReplicationMode::PredictiveInterpolation : EPhysicsReplicationMode::Default;
	if (GetPhysicsReplicationMode() != ReplicationMode) SetPhysicsReplicationMode(ReplicationMode);
	PhysicsBox->SetBoxExtent(PhysicsConfiguration.LocalExtent, false);
	PhysicsBox->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	if (bLooseDebris)
	{
		// 可见产品和物理盒保持同尺寸，否则触地和堆叠会使模型陷入支撑面。
		// 初始重叠由受限解穿透速度消解，不能通过缩小碰撞几何隐藏。
		PhysicsBox->CanCharacterStepUpOn = ECB_No;
		// 仅禁止 StepUp 不足以阻止 CharacterMovement 把顶面登记成移动 Base。
		// LooseDebris 会在脚下滚动，必须整体标记为不可行走表面，避免角色被刚体位姿带走。
		PhysicsBox->SetWalkableSlopeOverride(
			FWalkableSlopeOverride(WalkableSlope_Unwalkable, 0.0f));
		PhysicsBox->SetCollisionObjectType(ECC_PhysicsBody);
		PhysicsBox->SetCollisionResponseToAllChannels(ECR_Ignore);
		PhysicsBox->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		PhysicsBox->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		// 两端使用同一刚体形状与受控踢速；客户端预测接触，最终状态由服务器纠正。
		PhysicsBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		PhysicsBox->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		PhysicsBox->SetLinearDamping(0.45f);
		PhysicsBox->SetAngularDamping(0.65f);
		PhysicsBox->BodyInstance.SetMaxDepenetrationVelocity(
			LooseDebrisMaxDepenetrationVelocity);
		PhysicsMaterial->Friction = 0.85f;
		PhysicsMaterial->Restitution = 0.02f;
		// 默认只静止 4 个物理步就睡眠，会使预唤醒的 Actor 尚未到达客户端便再次销毁。
		// 连续静止 30 步再收回代理；陨石直接落成 Dormant 的大批物件不走此路径。
		PhysicsMaterial->SleepCounterThreshold = 30;
	}
	else
	{
		PhysicsBox->CanCharacterStepUpOn = ECB_Yes;
		PhysicsBox->SetWalkableSlopeOverride(FWalkableSlopeOverride());
		PhysicsBox->SetLinearDamping(
			UE::ElementSandbox::WorldObjects::Physics::DefaultLinearDamping);
		PhysicsBox->SetAngularDamping(
			UE::ElementSandbox::WorldObjects::Physics::DefaultAngularDamping);
		PhysicsBox->BodyInstance.SetOverrideMaxDepenetrationVelocity(false);
		PhysicsMaterial->Friction = UE::ElementSandbox::WorldObjects::Physics::DefaultFriction;
		PhysicsMaterial->Restitution =
			UE::ElementSandbox::WorldObjects::Physics::DefaultRestitution;
		PhysicsMaterial->SleepCounterThreshold = GetDefault<UPhysicalMaterial>()->SleepCounterThreshold;
	}
	PhysicsBox->SetPhysMaterialOverride(PhysicsMaterial);
	PhysicsBox->SetCollisionEnabled(HasAuthority() || bClientPhysicsProjectionActive
		? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	PhysicsBox->SetMassOverrideInKg(NAME_None, PhysicsConfiguration.MassKg, true);
	ApplyPhysicsReleaseState();
}

void AWorldObjectPhysicsProxyActor::ApplyPhysicsReleaseState()
{
	if (!PhysicsConfiguration.IsValid() || !IsValid(PhysicsBox))
	{
		return;
	}
	if (!HasAuthority() && !bClientPhysicsProjectionActive)
	{
		PhysicsBox->SetSimulatePhysics(false);
		return;
	}
	PhysicsBox->SetSimulatePhysics(bPhysicsReleased);
}

void AWorldObjectPhysicsProxyActor::ReleasePhysicsImmediately()
{
	if (!HasAuthority() || bPhysicsReleased || !PhysicsConfiguration.IsValid())
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(PhysicsReleaseTimer);
	bPhysicsReleased = true;
	ApplyPhysicsReleaseState();
	PhysicsBox->SetPhysicsLinearVelocity(PendingInitialLinearVelocity);
	PhysicsBox->SetPhysicsAngularVelocityInDegrees(PendingInitialAngularVelocityDegrees);
	PhysicsBox->WakeAllRigidBodies();
	ForceNetUpdate();
}

void AWorldObjectPhysicsProxyActor::RefreshClientPhysicsProjection()
{
	if (HasAuthority()) return;
	UWorldObjectWorldSubsystem* Objects = GetWorld()
		? GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>() : nullptr;
	const FWorldObjectEntityHandle Entity = WorldObjectProxyComponent->GetLocalEntity();
	const FWorldObjectMotionFragment* Motion = Objects
		? Objects->GetRegistry().FindFragment<FWorldObjectMotionFragment>(Entity) : nullptr;
	const FWorldObjectWorldIdentityFragment* Identity = Objects
		? Objects->GetRegistry().FindFragment<FWorldObjectWorldIdentityFragment>(Entity) : nullptr;
	if (PhysicsConfiguration.IsValid() && Identity && Motion
		&& Motion->State == EWorldObjectMotionState::Dormant
		&& Identity->StateRevision >= PhysicsConfiguration.ActivationRevision)
	{
		// Dormant Delta 可以先于 Actor 销毁到达；此刻旧刚体已经无权参与碰撞或显示。
		bClientPhysicsProjectionRetired = true;
	}
	const bool bActive = !bClientPhysicsProjectionRetired && PhysicsConfiguration.IsValid()
		&& Identity && Motion && Motion->State == EWorldObjectMotionState::Physics
		&& Identity->StateRevision >= PhysicsConfiguration.ActivationRevision
		&& Objects->GetProxy(Entity) == WorldObjectProxyComponent;
	SetClientPhysicsProjectionActive(bActive);
	if (bActive && bHasDeferredReplicatedMovement) OnRep_ReplicatedMovement();
}

void AWorldObjectPhysicsProxyActor::RetireClientPhysicsProjection()
{
	if (HasAuthority()) return;
	bClientPhysicsProjectionRetired = true;
	SetClientPhysicsProjectionActive(false);
}

void AWorldObjectPhysicsProxyActor::SetClientPhysicsProjectionActive(const bool bActive)
{
	const bool bChanged = bClientPhysicsProjectionActive != bActive;
	bClientPhysicsProjectionActive = bActive;
	if (!bActive)
	{
		if (bChanged && GetWorld()->GetPhysicsScene())
		{
			if (IPhysicsReplication* Replication = GetWorld()->GetPhysicsScene()->GetPhysicsReplication())
				Replication->RemoveReplicatedTarget(PhysicsBox);
		}
		PhysicsBox->SetSimulatePhysics(false);
		PhysicsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	else
	{
		PhysicsBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ApplyPhysicsReleaseState();
	}
	if (bChanged)
	{
		if (UWorldObjectCollisionWorldSubsystem* Collision =
			GetWorld()->GetSubsystem<UWorldObjectCollisionWorldSubsystem>())
			Collision->NotifyPhysicsProjectionChanged(WorldObjectProxyComponent->GetLocalEntity());
	}
}
