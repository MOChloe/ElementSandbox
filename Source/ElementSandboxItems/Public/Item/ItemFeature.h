#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ItemFeature.generated.h"

class UItemInstance;

/**
 * 道具能力与运行时数据的最小组成单元。
 * Feature 由 ItemInstance 独占，不能在多个道具实例之间共享。
 * 这是面向少量 UObject 的组合模型，不属于世界元素 ECS。
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class ELEMENTSANDBOXITEMS_API UItemFeature : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Runtime Feature 由 ItemInstance 从 DataAsset 模板复制产生。DuplicateObject 可能保留
	 * RF_WasLoaded，但客户端上的对应实例必须由 ActorChannel 动态创建，不能按模板路径解析。
	 */
	virtual bool IsNameStableForNetworking() const override { return false; }
	virtual bool IsSupportedForNetworking() const override { return true; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** ItemInstance 完成全部 Feature 复制后调用，供 Feature 建立仅依赖所属道具的初始状态。 */
	virtual void OnItemCreated(UItemInstance& ItemInstance) {}

protected:
	/** Feature 的复制属性变化后通知背包刷新 UI；不承担任何额外业务规则。 */
	void NotifyItemChanged() const;
};
