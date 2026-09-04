#pragma once

#include "Collision/WorldObjectCollisionTypes.h"
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"

#include "WorldObjectCollisionWorldSubsystem.generated.h"

class AActor;
class FWorldObjectCollisionData;
class UInstancedStaticMeshComponent;
class UPrimitiveComponent;
struct FWorldObjectQuerySnapshotBatch;

/** 普通 Dormant WorldObject 的角色近场 Box 碰撞投影；Gameplay 真值仍在 WorldObject ECS。 */
UCLASS()
class ELEMENTSANDBOXWORLDOBJECTS_API UWorldObjectCollisionWorldSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UWorldObjectCollisionWorldSubsystem();
	virtual ~UWorldObjectCollisionWorldSubsystem() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	FWorldObjectCollisionSourceHandle RegisterSource(const FWorldObjectCollisionSource& Source);
	bool UpdateSource(FWorldObjectCollisionSourceHandle Handle, const FWorldObjectCollisionSource& Source);
	bool UnregisterSource(FWorldObjectCollisionSourceHandle Handle);
	/** Source 或 WorldObject 生命周期变化后，在 CharacterMovement 前同步补齐 Immediate 碰撞。 */
	void FlushImmediateCollisionChanges();
	/** Client 的 Actor/Record 分路到达时同步转移碰撞所有权；只刷新该实体，不修改 ECS 状态。 */
	void NotifyPhysicsProjectionChanged(FWorldObjectEntityHandle Entity);
	/** Authority 将角色命中的 Dormant LooseDebris 实例提升为 Physics；质量参与速度传递。 */
	bool QueueLooseDebrisPawnContact(
		const UPrimitiveComponent& HitComponent,
		int32 InstanceIndex,
		const FVector& PawnVelocity);
	const FWorldObjectCollisionActivationConfig& GetActivationConfig() const;
	FWorldObjectCollisionStats GetStats() const;
	bool IsIdle() const;

private:
	void HandleSnapshotBatch(const FWorldObjectQuerySnapshotBatch& Batch);
	void ProcessPendingPawnContacts();
	void RefreshDirtySources();
	void ExpireGrace(double NowSeconds);
	void ApplyContinuityHandoffAdds(TConstArrayView<FWorldObjectEntityHandle> Entities);
	void ApplyAdds(int32 Budget, bool bImmediateOnly);
	bool SubmitAddBatch(
		UInstancedStaticMeshComponent* Instances,
		TArray<FWorldObjectEntityHandle>& Owners,
		TConstArrayView<FWorldObjectEntityHandle> Entities,
		TConstArrayView<FTransform> Transforms);
	void ApplyRemoves(int32 Budget);
	void RemoveCollisionInstance(FWorldObjectEntityHandle Entity);
	bool EnsureCollisionHost();
	void ReleaseCollisionHost();

	TPimplPtr<FWorldObjectCollisionData> Data;

	UPROPERTY(Transient)
	TObjectPtr<AActor> CollisionHost = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> StandardCollisionInstances = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> LooseDebrisCollisionInstances = nullptr;

	FDelegateHandle SnapshotBatchHandle;
};
