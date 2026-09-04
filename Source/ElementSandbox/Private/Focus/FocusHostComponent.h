#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Focus/FocusQueryTypes.h"

#include "FocusHostComponent.generated.h"

class UFocusHandler;
struct FFocusInteractionPrompt;

/**
 * 本地玩家的 Focus 仲裁器。
 * 它构造 View Context、调用已注册 Query、按直接命中/辅助优先级选 Hit，并在目标变化时向该
 * Query 的 Handler 发送一次 Gained/Lost。没有注册项时不会产生任何 Focus 数据。
 */
UCLASS(NotBlueprintable, ClassGroup = (Focus))
class UFocusHostComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UFocusHostComponent();

	/** Query 与能解释其 Target 的 Handler 成对注册；Owner 失效后自动清理。 */
	FFocusQueryRegistrationHandle RegisterQuery(
		UObject& Owner,
		FFocusQueryDelegate Query,
		UFocusHandler& Handler);

	bool UnregisterQuery(FFocusQueryRegistrationHandle Handle);

	/** 供 Tick 和确定性测试共用；调用结束后最多保留一个当前目标。 */
	void EvaluateFocus(const FFocusQueryContext& Context);

	void ClearFocus();
	/** 持续交互期间过滤未显式声明可重复的候选，防止拾取输入转成开关门。 */
	void SetRepeatableInteractOnly(bool bEnabled);
	bool HandleInteract();
	/** 将装备道具的主要 Use 交给当前目标；未处理时调用方可以继续走 Ability 输入。 */
	bool HandlePrimaryUse();
	bool TryResolveFocusedPrompt(FFocusInteractionPrompt& OutPrompt) const;

	const FFocusQueryHit* GetFocusedHit() const;
	int32 GetRegisteredQueryCount() const { return Registrations.Num(); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FRegistration final
	{
		FFocusQueryRegistrationHandle Handle;
		TWeakObjectPtr<UObject> Owner;
		FFocusQueryDelegate Query;
		TWeakObjectPtr<UFocusHandler> Handler;
	};

	struct FCandidate final
	{
		FFocusQueryRegistrationHandle Registration;
		FFocusQueryHit Hit;
	};

	bool BuildViewContext(FFocusQueryContext& OutContext) const;
	const FRegistration* FindRegistration(FFocusQueryRegistrationHandle Handle) const;
	void ApplyFocus(FCandidate&& Candidate);
	void PruneInvalidRegistrations();

	TArray<FRegistration> Registrations;

	UPROPERTY(Transient)
	TArray<FFocusQueryHit> QueryHitScratch;

	uint64 NextRegistrationValue = 1;
	bool bEvaluatingQueries = false;
	bool bDispatchingFocusChange = false;

	UPROPERTY(Transient)
	FFocusQueryHit FocusedHit;

	FFocusQueryRegistrationHandle FocusedRegistration;
	bool bHasFocusedHit = false;
	bool bRepeatableInteractOnly = false;
};
