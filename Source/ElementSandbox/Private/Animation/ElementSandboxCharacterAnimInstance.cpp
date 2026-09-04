#include "Animation/ElementSandboxCharacterAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNode_LinkedAnimGraph.h"
#include "Animation/AnimNode_SaveCachedPose.h"
#include "Animation/AnimNodeSpaceConversions.h"
#include "Animation/AnimNode_UseCachedPose.h"
#include "Animation/AnimSequenceBase.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_Slot.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
#include "GameFramework/Pawn.h"
#include "UObject/ConstructorHelpers.h"

namespace ElementSandbox::Animation
{
	const FName UpperBodySlotName(TEXT("UpperBody"));
	const FName LocomotionCacheName(TEXT("Locomotion"));
	const FName UpperBodyRootBone(TEXT("spine_01"));
	const FName MiddleSpineBone(TEXT("spine_02"));
	const FName UpperSpineBone(TEXT("spine_03"));
	constexpr float MaxAimPitch = 45.0f;
	constexpr float AimInterpolationSpeed = 12.0f;
}

/**
 * 用原生 AnimNode 组装固定的姿势管线：
 * Existing Locomotion AnimBP -> Cached Pose -> UpperBody Slot -> Per-Bone Blend。
 *
 * 节点拓扑集中在此处，业务代码只看 UElementSandboxCharacterAnimInstance 的播放接口。
 */
class FElementSandboxCharacterAnimInstanceProxy final : public FAnimInstanceProxy
{
public:
	explicit FElementSandboxCharacterAnimInstanceProxy(UElementSandboxCharacterAnimInstance* AnimInstance)
		: FAnimInstanceProxy(AnimInstance)
	{
		LinkedLocomotion.InstanceClass = AnimInstance->GetLocomotionAnimClass();
		LinkedLocomotion.bReceiveNotifiesFromLinkedInstances = true;
		LinkedLocomotion.bPropagateNotifiesToLinkedInstances = true;

		LocomotionCache.CachePoseName = ElementSandbox::Animation::LocomotionCacheName;
		LocomotionCache.Pose.SetLinkNode(&LinkedLocomotion);

		BaseLocomotion.CachePoseName = ElementSandbox::Animation::LocomotionCacheName;
		BaseLocomotion.LinkToCachingNode.SetLinkNode(&LocomotionCache);
		UpperBodySource.CachePoseName = ElementSandbox::Animation::LocomotionCacheName;
		UpperBodySource.LinkToCachingNode.SetLinkNode(&LocomotionCache);

		UpperBodySlot.SlotName = ElementSandbox::Animation::UpperBodySlotName;
		UpperBodySlot.bAlwaysUpdateSourcePose = true;
		UpperBodySlot.Source.SetLinkNode(&UpperBodySource);

		UpperBodyBlend.BasePose.SetLinkNode(&BaseLocomotion);
		UpperBodyBlend.AddPose();
		UpperBodyBlend.BlendPoses[0].SetLinkNode(&UpperBodySlot);
		UpperBodyBlend.BlendWeights[0] = 1.0f;
		UpperBodyBlend.bMeshSpaceRotationBlend = true;
		FBranchFilter& UpperBodyFilter = UpperBodyBlend.LayerSetup[0].BranchFilters.AddDefaulted_GetRef();
		UpperBodyFilter.BoneName = ElementSandbox::Animation::UpperBodyRootBone;
		UpperBodyFilter.BlendDepth = 0;

		LocalToComponent.LocalPose.SetLinkNode(&UpperBodyBlend);
		ConfigureAimBone(LowerSpineAim, ElementSandbox::Animation::UpperBodyRootBone);
		LowerSpineAim.ComponentPose.SetLinkNode(&LocalToComponent);
		ConfigureAimBone(MiddleSpineAim, ElementSandbox::Animation::MiddleSpineBone);
		MiddleSpineAim.ComponentPose.SetLinkNode(&LowerSpineAim);
		ConfigureAimBone(UpperSpineAim, ElementSandbox::Animation::UpperSpineBone);
		UpperSpineAim.ComponentPose.SetLinkNode(&MiddleSpineAim);
		ComponentToLocal.ComponentPose.SetLinkNode(&UpperSpineAim);
	}

	virtual FAnimNode_Base* GetCustomRootNode() override
	{
		return &ComponentToLocal;
	}

	virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override
	{
		OutNodes.Add(&LinkedLocomotion);
		OutNodes.Add(&LocomotionCache);
		OutNodes.Add(&BaseLocomotion);
		OutNodes.Add(&UpperBodySource);
		OutNodes.Add(&UpperBodySlot);
		OutNodes.Add(&UpperBodyBlend);
		OutNodes.Add(&LocalToComponent);
		OutNodes.Add(&LowerSpineAim);
		OutNodes.Add(&MiddleSpineAim);
		OutNodes.Add(&UpperSpineAim);
		OutNodes.Add(&ComponentToLocal);
	}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, const float DeltaSeconds) override
	{
		FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
		TargetAimPitch = 0.0f;
		const UElementSandboxCharacterAnimInstance* CharacterAnimInstance =
			Cast<UElementSandboxCharacterAnimInstance>(InAnimInstance);
		const APawn* Pawn = CharacterAnimInstance ? CharacterAnimInstance->TryGetPawnOwner() : nullptr;
		if (Pawn)
		{
			const FRotator RelativeAim = (Pawn->GetBaseAimRotation() - Pawn->GetActorRotation()).GetNormalized();
			TargetAimPitch = FMath::Clamp(
				RelativeAim.Pitch,
				-ElementSandbox::Animation::MaxAimPitch,
				ElementSandbox::Animation::MaxAimPitch);
		}
	}

	virtual void UpdateAnimationNode(const FAnimationUpdateContext& InContext) override
	{
		SmoothedAimPitch = FMath::FInterpTo(
			SmoothedAimPitch,
			TargetAimPitch,
			InContext.GetDeltaTime(),
			ElementSandbox::Animation::AimInterpolationSpeed);

		// 分散到三节脊柱，避免只折 spine_01 产生机械的“腰部铰链”效果。
		// Quinn Mesh 相对角色 Yaw=-90°：角色的俯仰轴在 Mesh Component Space 中对应 X/Roll。
		// 使用固定 Component Space，避免多节脊柱在不断变化的 Bone Space 局部轴上累计成侧弯。
		LowerSpineAim.Rotation = FRotator(0.0f, 0.0f, -SmoothedAimPitch * 0.25f);
		MiddleSpineAim.Rotation = FRotator(0.0f, 0.0f, -SmoothedAimPitch * 0.35f);
		UpperSpineAim.Rotation = FRotator(0.0f, 0.0f, -SmoothedAimPitch * 0.40f);

		FAnimInstanceProxy::UpdateAnimationNode(InContext);
		// 原生代理没有 AnimBlueprint 编译器生成的 CachedPose 队列，需要显式结束本帧缓存更新。
		LocomotionCache.PostGraphUpdate();
	}

private:
	static void ConfigureAimBone(FAnimNode_ModifyBone& Node, const FName BoneName)
	{
		Node.BoneToModify.BoneName = BoneName;
		Node.TranslationMode = BMM_Ignore;
		Node.RotationMode = BMM_Additive;
		Node.ScaleMode = BMM_Ignore;
		Node.RotationSpace = BCS_ComponentSpace;
		Node.Alpha = 1.0f;
	}

	FAnimNode_LinkedAnimGraph LinkedLocomotion;
	FAnimNode_SaveCachedPose LocomotionCache;
	FAnimNode_UseCachedPose BaseLocomotion;
	FAnimNode_UseCachedPose UpperBodySource;
	FAnimNode_Slot UpperBodySlot;
	FAnimNode_LayeredBoneBlend UpperBodyBlend;
	FAnimNode_ConvertLocalToComponentSpace LocalToComponent;
	FAnimNode_ModifyBone LowerSpineAim;
	FAnimNode_ModifyBone MiddleSpineAim;
	FAnimNode_ModifyBone UpperSpineAim;
	FAnimNode_ConvertComponentToLocalSpace ComponentToLocal;
	float TargetAimPitch = 0.0f;
	float SmoothedAimPitch = 0.0f;
};

UElementSandboxCharacterAnimInstance::UElementSandboxCharacterAnimInstance()
{
	static ConstructorHelpers::FClassFinder<UAnimInstance> LocomotionBlueprint(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	LocomotionAnimClass = LocomotionBlueprint.Class;
}

UAnimMontage* UElementSandboxCharacterAnimInstance::PlayUpperBodyAnimation(
	UAnimSequenceBase* Animation,
	const float BlendInTime,
	const float BlendOutTime,
	const float PlayRate)
{
	if (!IsValid(Animation) || PlayRate <= 0.0f)
	{
		return nullptr;
	}

	UAnimMontage* DynamicMontage = PlaySlotAnimationAsDynamicMontage(
		Animation,
		ElementSandbox::Animation::UpperBodySlotName,
		FMath::Max(0.0f, BlendInTime),
		FMath::Max(0.0f, BlendOutTime),
		PlayRate,
		1);
	if (DynamicMontage)
	{
		// 上半身纯表现动作不能让源动画的 Root Motion 接管 CharacterMovement。
		if (FAnimMontageInstance* MontageInstance = GetActiveInstanceForMontage(DynamicMontage))
		{
			MontageInstance->PushDisableRootMotion();
		}
	}
	return DynamicMontage;
}

void UElementSandboxCharacterAnimInstance::StopUpperBodyAnimation(const float BlendOutTime)
{
	StopSlotAnimation(FMath::Max(0.0f, BlendOutTime), ElementSandbox::Animation::UpperBodySlotName);
}

FName UElementSandboxCharacterAnimInstance::GetUpperBodySlotName()
{
	return ElementSandbox::Animation::UpperBodySlotName;
}

FAnimInstanceProxy* UElementSandboxCharacterAnimInstance::CreateAnimInstanceProxy()
{
	return new FElementSandboxCharacterAnimInstanceProxy(this);
}

void UElementSandboxCharacterAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete InProxy;
}
