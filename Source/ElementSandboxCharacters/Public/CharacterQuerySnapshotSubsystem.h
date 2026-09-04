#pragma once

#include "CharacterQuerySnapshotTypes.h"
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "CharacterQuerySnapshotSubsystem.generated.h"

class ACharacter;
class FCharacterQuerySnapshotRuntime;
class UAbilitySystemComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(
	FCharacterQuerySnapshotsCommittedEvent,
	const FCharacterQuerySnapshotBatch&);

/**
 * 普通 Character Actor 的 Post-Actor 查询投影。它不是 ECS：没有 Registry、Fragment Pool、
 * Spatial Grid 或 100m Journal，只冻结连续 Capsule POD 并发布前后运动批次。
 */
UCLASS()
class ELEMENTSANDBOXCHARACTERS_API UCharacterQuerySnapshotSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UCharacterQuerySnapshotSubsystem();
	virtual ~UCharacterQuerySnapshotSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool HasRuntimeState() const { return Runtime.IsValid(); }
	FCharacterSnapshotHandle RegisterCharacter(ACharacter& Character, UAbilitySystemComponent& AbilitySystem);
	bool UnregisterCharacter(FCharacterSnapshotHandle Handle);

	FCharacterSnapshotHandle FindSnapshot(const ACharacter& Character) const;
	/** Remove 批次同步广播期间仍可解析正在注销的对象，供宿主无残留地撤销投影。 */
	ACharacter* ResolveCharacter(FCharacterSnapshotHandle Handle) const;
	UAbilitySystemComponent* ResolveAbilitySystem(FCharacterSnapshotHandle Handle) const;
	bool CopySnapshot(FCharacterSnapshotHandle Handle, FCharacterQuerySnapshot& OutSnapshot) const;
	void CopyAllSnapshots(TArray<FCharacterQuerySnapshot>& OutSnapshots) const;

	/** 每个引擎帧最多采样一次；变化批次在 Post-Actor 同步回调中发布。 */
	void EnsurePostActorSnapshotsCurrent();
	FCharacterQuerySnapshotsCommittedEvent& OnSnapshotsCommitted() { return SnapshotsCommittedEvent; }
	FCharacterQuerySnapshotStats GetStats() const;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	TPimplPtr<FCharacterQuerySnapshotRuntime> Runtime;
	FCharacterQuerySnapshotsCommittedEvent SnapshotsCommittedEvent;
};
