// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/ElementSandboxCharacter.h"
#include "Characters/ElementSandboxCharacterMovementComponent.h"
#include "Abilities/EquipmentAbilityBridgeComponent.h"
#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Characters/CharacterBurningPresentationComponent.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "InputAction.h"
#include "Animation/ElementSandboxCharacterAnimInstance.h"
#include "Animation/AnimSequenceBase.h"
#include "CharacterQuerySnapshotSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "ElementSandbox.h"
#include "Equipment/EquipmentComponent.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Game/ElementSandboxPlayerController.h"
#include "WorldObjects/WorldObjectEquipmentBridgeComponent.h"

AElementSandboxCharacter::AElementSandboxCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UElementSandboxCharacterMovementComponent>(
		ACharacter::CharacterMovementComponentName))
{
	// 保持胶囊、移动和镜头参数集中在角色默认对象中，避免依赖派生蓝图。
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
	GetCapsuleComponent()->SetCastShadow(false);
	GetCapsuleComponent()->bCastDynamicShadow = false;
	GetCapsuleComponent()->bCastStaticShadow = false;
	GetMesh()->SetCastShadow(false);
	GetMesh()->bCastDynamicShadow = false;
	GetMesh()->bCastStaticShadow = false;

	bUseControllerRotationPitch = false;
	// 生存建造操作：鼠标水平朝向就是角色朝向，相机不再绕角色自由甩动。
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	// WASD 只决定相对当前视角的前后左右，不允许移动方向覆盖鼠标确定的朝向。
	UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	MovementComponent->bOrientRotationToMovement = false;
	MovementComponent->JumpZVelocity = 500.f;
	MovementComponent->AirControl = 0.35f;
	MovementComponent->MaxWalkSpeed = 500.f;
	MovementComponent->MinAnalogWalkSpeed = 20.f;
	MovementComponent->BrakingDecelerationWalking = 2000.f;
	MovementComponent->BrakingDecelerationFalling = 1500.0f;
	// 可动落脚面仍提供位移支撑，但不能接管玩家朝向和镜头旋转。
	MovementComponent->bIgnoreBaseRotation = true;

	// UE 默认持续推力 750000 会让十几公斤的刚体在一次角色碰撞后获得离谱速度。
	// 使用恒定而非按质量放大的力，让轻物仍易推动、重物自然体现惯性。
	MovementComponent->bEnablePhysicsInteraction = true;
	MovementComponent->InitialPushForceFactor = 500.0f;
	MovementComponent->PushForceFactor = 10000.0f;
	MovementComponent->bPushForceScaledToMass = false;
	MovementComponent->bPushForceUsingZOffset = true;
	MovementComponent->PushForcePointZOffsetFactor = 0.0f;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	EquipmentComponent = CreateDefaultSubobject<UEquipmentComponent>(TEXT("EquipmentComponent"));
	EquipmentAbilityBridgeComponent = CreateDefaultSubobject<UEquipmentAbilityBridgeComponent>(
		TEXT("EquipmentAbilityBridgeComponent"));
	WorldObjectEquipmentBridgeComponent =
		CreateDefaultSubobject<UWorldObjectEquipmentBridgeComponent>(
			TEXT("WorldObjectEquipmentBridgeComponent"));
		static ConstructorHelpers::FObjectFinder<USkeletalMesh> CharacterMesh(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
	if (CharacterMesh.Succeeded())
	{
		GetMesh()->SetSkeletalMeshAsset(CharacterMesh.Object);
		GetMesh()->SetRelativeLocation(FVector(0.0f, 0.0f, -90.0f));
		GetMesh()->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
	}

	GetMesh()->SetAnimInstanceClass(UElementSandboxCharacterAnimInstance::StaticClass());

	static ConstructorHelpers::FObjectFinder<UInputAction> JumpInput(
		TEXT("/Game/Input/Actions/IA_Jump.IA_Jump"));
	static ConstructorHelpers::FObjectFinder<UInputAction> MoveInput(
		TEXT("/Game/Input/Actions/IA_Move.IA_Move"));
	static ConstructorHelpers::FObjectFinder<UInputAction> LookInput(
		TEXT("/Game/Input/Actions/IA_Look.IA_Look"));
	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookInput(
		TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook"));

	JumpAction = JumpInput.Object;
	MoveAction = MoveInput.Object;
	LookAction = LookInput.Object;
	MouseLookAction = MouseLookInput.Object;
}

void AElementSandboxCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitializeAbilityActorInfo();
}

void AElementSandboxCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitializeAbilityActorInfo();
}

void AElementSandboxCharacter::UnPossessed()
{
	ClearAbilityActorInfo();
	Super::UnPossessed();
}

void AElementSandboxCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearAbilityActorInfo();
	Super::EndPlay(EndPlayReason);
}

void AElementSandboxCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AElementSandboxCharacter::Move);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AElementSandboxCharacter::MouseLook);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AElementSandboxCharacter::Look);
	}
	else
	{
		UE_LOG(LogElementSandbox, Error, TEXT("%s 未找到 Enhanced Input 组件。"), *GetNameSafe(this));
	}
}

void AElementSandboxCharacter::Move(const FInputActionValue& Value)
{
	if (Controller != nullptr)
	{
		const FVector2D MovementVector = Value.Get<FVector2D>();
		const FRotator YawRotation(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AElementSandboxCharacter::Look(const FInputActionValue& Value)
{
	if (Controller != nullptr)
	{
		const FVector2D LookAxisVector = Value.Get<FVector2D>();
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

void AElementSandboxCharacter::MouseLook(const FInputActionValue& Value)
{
	if (AElementSandboxPlayerController* SandboxController =
		Cast<AElementSandboxPlayerController>(Controller))
	{
		SandboxController->ApplyMouseLookInput(Value.Get<FVector2D>());
	}
}

bool AElementSandboxCharacter::PlayPredictedUpperBodyAnimation(
	UAnimSequenceBase* Animation,
	const float BlendInTime,
	const float BlendOutTime,
	const float PlayRate)
{
	if (!IsValid(Animation) || PlayRate <= 0.0f)
	{
		return false;
	}

	bool bAccepted = false;
	if (IsLocallyControlled() && GetNetMode() != NM_DedicatedServer)
	{
		bAccepted = PlayUpperBodyAnimationLocally(Animation, BlendInTime, BlendOutTime, PlayRate);
	}
	if (HasAuthority())
	{
		MulticastPlayUpperBodyAnimation(Animation, BlendInTime, BlendOutTime, PlayRate);
		bAccepted = true;
	}
	return bAccepted;
}

void AElementSandboxCharacter::StopPredictedUpperBodyAnimation(const float BlendOutTime)
{
	if (IsLocallyControlled() && GetNetMode() != NM_DedicatedServer)
	{
		StopUpperBodyAnimationLocally(BlendOutTime);
	}
	if (HasAuthority())
	{
		MulticastStopUpperBodyAnimation(BlendOutTime);
	}
}

void AElementSandboxCharacter::MulticastPlayUpperBodyAnimation_Implementation(
	UAnimSequenceBase* Animation,
	const float BlendInTime,
	const float BlendOutTime,
	const float PlayRate)
{
	// 所属客户端已经预测播放，收到服务器广播时不能从头重播。
	if (GetNetMode() == NM_DedicatedServer || IsLocallyControlled())
	{
		return;
	}

	PlayUpperBodyAnimationLocally(Animation, BlendInTime, BlendOutTime, PlayRate);
}

void AElementSandboxCharacter::MulticastStopUpperBodyAnimation_Implementation(const float BlendOutTime)
{
	if (GetNetMode() == NM_DedicatedServer || IsLocallyControlled())
	{
		return;
	}
	StopUpperBodyAnimationLocally(BlendOutTime);
}

void AElementSandboxCharacter::InitializeAbilityActorInfo()
{
	AElementSandboxPlayerState* SandboxPlayerState = GetPlayerState<AElementSandboxPlayerState>();
	UElementAbilitySystemComponent* AbilitySystem = SandboxPlayerState
		? SandboxPlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	if (!AbilitySystem)
	{
		return;
	}

		AbilitySystem->InitAbilityActorInfo(SandboxPlayerState, this);
		EnsureBurningPresentation();
		if (BurningPresentationComponent)
		{
			BurningPresentationComponent->InitializeAbilitySystem(AbilitySystem);
		}
	UWorld* World = GetWorld();
	if (UCharacterQuerySnapshotSubsystem* CharacterSubsystem = World
		? World->GetSubsystem<UCharacterQuerySnapshotSubsystem>()
		: nullptr)
	{
		if (CharacterSnapshotHandle.IsSet()
			&& (CharacterSubsystem->ResolveCharacter(CharacterSnapshotHandle) != this
				|| CharacterSubsystem->ResolveAbilitySystem(CharacterSnapshotHandle)
					!= AbilitySystem))
		{
			CharacterSubsystem->UnregisterCharacter(CharacterSnapshotHandle);
			CharacterSnapshotHandle = {};
		}

		const FCharacterSnapshotHandle RegisteredEntity =
			CharacterSubsystem->RegisterCharacter(*this, *AbilitySystem);
		const bool bFeatureProjectionReady = RegisteredEntity.IsSet();
		if (bFeatureProjectionReady)
		{
			CharacterSnapshotHandle = RegisteredEntity;
		}
		else if (RegisteredEntity.IsSet())
		{
			CharacterSubsystem->UnregisterCharacter(RegisteredEntity);
			CharacterSnapshotHandle = {};
		}
	}
		EquipmentAbilityBridgeComponent->InitializeAbilitySystem(AbilitySystem);
}

void AElementSandboxCharacter::EnsureBurningPresentation()
{
	if (GetNetMode() == NM_DedicatedServer || BurningPresentationComponent)
	{
		return;
	}

	BurningPresentationComponent = NewObject<UCharacterBurningPresentationComponent>(
		this,
		TEXT("BurningPresentationComponent"));
	AddInstanceComponent(BurningPresentationComponent);
	BurningPresentationComponent->SetupAttachment(GetCapsuleComponent());
	BurningPresentationComponent->RegisterComponent();
}

void AElementSandboxCharacter::ClearAbilityActorInfo()
{
	if (BurningPresentationComponent)
	{
		BurningPresentationComponent->InitializeAbilitySystem(nullptr);
	}

	if (CharacterSnapshotHandle.IsSet())
	{
		if (UWorld* World = GetWorld())
		{
			if (UCharacterQuerySnapshotSubsystem* CharacterSubsystem =
				World->GetSubsystem<UCharacterQuerySnapshotSubsystem>())
			{
				CharacterSubsystem->UnregisterCharacter(CharacterSnapshotHandle);
			}
		}
		CharacterSnapshotHandle = {};
	}

	if (EquipmentAbilityBridgeComponent)
	{
		EquipmentAbilityBridgeComponent->InitializeAbilitySystem(nullptr);
	}

	AElementSandboxPlayerState* SandboxPlayerState = GetPlayerState<AElementSandboxPlayerState>();
	UElementAbilitySystemComponent* AbilitySystem = SandboxPlayerState
		? SandboxPlayerState->GetElementAbilitySystemComponent()
		: nullptr;
	if (AbilitySystem && AbilitySystem->GetAvatarActor() == this)
	{
		AbilitySystem->ClearActorInfo();
	}
}

bool AElementSandboxCharacter::PlayUpperBodyAnimationLocally(
	UAnimSequenceBase* Animation,
	const float BlendInTime,
	const float BlendOutTime,
	const float PlayRate) const
{
	if (!IsValid(GetMesh()))
	{
		return false;
	}

	if (UElementSandboxCharacterAnimInstance* AnimInstance =
		Cast<UElementSandboxCharacterAnimInstance>(GetMesh()->GetAnimInstance()))
	{
		return AnimInstance->PlayUpperBodyAnimation(Animation, BlendInTime, BlendOutTime, PlayRate) != nullptr;
	}
	return false;
}

void AElementSandboxCharacter::StopUpperBodyAnimationLocally(const float BlendOutTime) const
{
	if (IsValid(GetMesh()))
	{
		if (UElementSandboxCharacterAnimInstance* AnimInstance =
			Cast<UElementSandboxCharacterAnimInstance>(GetMesh()->GetAnimInstance()))
		{
			AnimInstance->StopUpperBodyAnimation(BlendOutTime);
		}
	}
}
