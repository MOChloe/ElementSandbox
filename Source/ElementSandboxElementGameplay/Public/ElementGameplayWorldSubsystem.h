#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldEntityId.h"
#include "Subsystems/WorldSubsystem.h"

#include "ElementGameplayWorldSubsystem.generated.h"

struct ELEMENTSANDBOXELEMENTGAMEPLAY_API FElementRuntimeFireSourceHandle final
{
	uint64 Id = 0;
	uint32 Generation = 0;

	bool IsSet() const { return Id != 0 && Generation != 0; }

	friend bool operator==(
		const FElementRuntimeFireSourceHandle& Left,
		const FElementRuntimeFireSourceHandle& Right)
	{
		return Left.Id == Right.Id && Left.Generation == Right.Generation;
	}

	friend uint32 GetTypeHash(const FElementRuntimeFireSourceHandle& Handle)
	{
		return HashCombineFast(GetTypeHash(Handle.Id), GetTypeHash(Handle.Generation));
	}
};

class FElementFireDomain;

struct FElementFireDomainDeleter
{
	void operator()(FElementFireDomain* Runtime) const;
};

/** Fire Rule、宿主 Adapter、Resolver、持久化与 GAS Outbox 的正式装配边界。 */
UCLASS()
class ELEMENTSANDBOXELEMENTGAMEPLAY_API UElementGameplayWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UElementGameplayWorldSubsystem();
	virtual ~UElementGameplayWorldSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	bool IsRuntimeAssemblyActive() const { return bRuntimeAssemblyActive; }

	/** Fireball 命中后按唯一 RuleSet 创建短寿命 RuntimeOnly Sphere；只允许 Authority 调用。 */
	FElementRuntimeFireSourceHandle CreateFireballSource(const FVector& WorldLocation);
	int64 GetFireballSourceLifetimeMilliseconds() const;
	bool RemoveRuntimeFireSource(FElementRuntimeFireSourceHandle Handle);

	/** 木棍主动接火/挥动窗口；仅为运行期交互事实，下一 Barrier 刷新传播策略。 */
	bool SetStickFireInteractionState(FWorldEntityId WorldEntityId, bool bActive);

#if WITH_DEV_AUTOMATION_TESTS
	bool IsStickFireInteractionActiveForTesting(FWorldEntityId WorldEntityId) const;
	int32 GetBuildingFireHostCountForTesting() const;
#endif

private:
	TUniquePtr<FElementFireDomain, FElementFireDomainDeleter> Runtime;
	bool bRuntimeAssemblyActive = false;
};
