#pragma once

#include "Entity/ElementFragment.h"
#include "Fire/ElementFireRuleSet.h"
#include "Processing/ElementProcessor.h"

/** 宿主 Adapter 写入目标快照的中性可燃物分类。 */
enum class EElementFireTargetProfile : uint8
{
	None,
	Structure,
	Stick,
	Character
};

/** Fire 的实例数据。Shape/强度属于实例；查询、调度和关系生命周期不属于 Fire。 */
struct FFireSourceFragment final : FElementInfluenceFragment
{
	double Intensity = 0.0;
	/** 原始作用距离；Shape 保留未扩张源几何，Capture 时才生成 Broadphase 支持体。 */
	double RangeCentimeters = 0.0;
	EFirePropagationPolicy Policy = EFirePropagationPolicy::All;
	/** 0 表示不自动到期；绝对世界时间只由装配层用于登记一次性唤醒。 */
	int64 ExpireTimeMilliseconds = 0;
	/** 由燃烧目标生成的 Source 记录宿主；固定火源和 Fireball 可以为空。 */
	FElementTargetKey HostTarget;

	bool IsValid() const;
};

namespace ElementFireRuntimeNames
{
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName NumericProcessor;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName StateProcessor;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName ThermalInput;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName CurrentFireRate;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName ThermalState;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName Projection;
	ELEMENTSANDBOXELEMENTGAMEPLAY_API extern const FName FireSourceFragment;
}

/** 把集中查询统计转换为可交换的 Thermal Offset；不访问宿主或 UObject。 */
class FElementFireNumericProcessor final : public FElementNumericProcessor
{
public:
	FElementFireNumericProcessor();

	virtual const FElementProcessorDescriptor& GetDescriptor() const override { return Descriptor; }
	virtual bool CaptureInfluence(
		const FElementEntityRegistry& Registry,
		FElementEntityHandle Source,
		FElementInfluenceSnapshot& OutSnapshot) const override;
	virtual void Execute(
		TConstArrayView<FElementQueryStatistics> Statistics,
		TArray<FElementOffset>& OutOffsets) const override;

private:
	FElementProcessorDescriptor Descriptor;
};

/** 唯一拥有 Thermal State 的公式处理器；只读归并后的数值并输出状态/结构/投影命令。 */
class FElementThermalStateProcessor final : public FElementStateProcessor
{
public:
	explicit FElementThermalStateProcessor(const FFireRuleSnapshot& InRules);

	virtual const FElementProcessorDescriptor& GetDescriptor() const override { return Descriptor; }
	virtual bool Execute(
		const FElementStateProcessorInput& Input,
		FElementStateProcessorOutput& OutOutput) const override;

private:
	const FFireCombustionProfile* SelectProfile(EElementFireTargetProfile Profile) const;

	FFireRuleSnapshot Rules;
	FElementProcessorDescriptor Descriptor;
};

ELEMENTSANDBOXELEMENTGAMEPLAY_API FElementValuePayload MakeFireTargetMetadata(
	EElementFireTargetProfile Profile,
	bool bFireInteractionActive = false);
ELEMENTSANDBOXELEMENTGAMEPLAY_API EElementFireTargetProfile ReadFireTargetMetadata(
	const FElementValuePayload& Metadata);
