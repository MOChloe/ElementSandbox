#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "ElementSandboxCharacterAnimInstance.generated.h"

class UAnimMontage;
class UAnimSequenceBase;

/**
 * 角色通用动画出口。
 *
 * 现有 AnimBP 继续负责移动状态机；本类只增加一个与具体道具无关的上半身动作通道。
 * 木棍、弓箭等行为只提交动画资产，不允许把具体道具判断写进角色动画层。
 *
 * 这段原生 AnimGraph 是交给 AI 代写的样板适配代码。它用于省去手工搭建固定 AnimBP
 * 节点的时间，不是 demo 的核心技术论点；如果未来采用完整动画框架，可以整体替换。
 */
UCLASS(Transient)
class UElementSandboxCharacterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UElementSandboxCharacterAnimInstance();

	/** 在通用上半身通道播放一次动作；调用方决定动画含义与网络策略。 */
	UAnimMontage* PlayUpperBodyAnimation(
		UAnimSequenceBase* Animation,
		float BlendInTime = 0.1f,
		float BlendOutTime = 0.15f,
		float PlayRate = 1.0f);

	/** 停止当前上半身动态 Montage。 */
	void StopUpperBodyAnimation(float BlendOutTime = 0.15f);

	static FName GetUpperBodySlotName();
	TSubclassOf<UAnimInstance> GetLocomotionAnimClass() const { return LocomotionAnimClass; }

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	/** 被原生混合层包装的项目移动 AnimBP；不由道具行为修改。 */
	UPROPERTY()
	TSubclassOf<UAnimInstance> LocomotionAnimClass;
};
