#include "CharacterQuerySnapshotSubsystem.h"

#include "AbilitySystemComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Subsystems/SubsystemCollection.h"
#include "WorldStorageSubsystem.h"

namespace
{
	TAtomic<uint32> NextCharacterSnapshotRegistryId{1};

	uint32 AllocateRegistryId()
	{
		uint32 Id = NextCharacterSnapshotRegistryId++;
		if (Id == 0) Id = NextCharacterSnapshotRegistryId++;
		return Id;
	}

	uint32 AdvanceGeneration(const uint32 Generation)
	{
		return Generation == MAX_uint32 ? 1 : Generation + 1;
	}

	uint64 AdvanceRevision(const uint64 Revision)
	{
		return Revision == MAX_uint64 ? 1 : Revision + 1;
	}

	bool CaptureCapsule(ACharacter& Character, FCharacterCapsuleSnapshot& OutCapsule)
	{
		const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
		if (!IsValid(Capsule)) return false;
		OutCapsule = {};
		OutCapsule.Center = Capsule->GetComponentLocation();
		OutCapsule.Axis = Capsule->GetUpVector().GetSafeNormal();
		OutCapsule.Radius = Capsule->GetScaledCapsuleRadius();
		OutCapsule.HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		return OutCapsule.IsValid();
	}
}

class FCharacterQuerySnapshotRuntime final
{
public:
	struct FSlot final
	{
		uint32 Generation = 1;
		int32 DenseIndex = INDEX_NONE;
		bool bAlive = false;
	};

	struct FResolver final
	{
		TWeakObjectPtr<ACharacter> Character;
		TWeakObjectPtr<UAbilitySystemComponent> AbilitySystem;
	};

	bool IsAlive(const FCharacterSnapshotHandle Handle) const
	{
		return Handle.GetRegistryId() == RegistryId && Slots.IsValidIndex(Handle.GetSlot())
			&& Slots[Handle.GetSlot()].bAlive
			&& Slots[Handle.GetSlot()].Generation == Handle.GetGeneration();
	}

	int64 GetEffectiveTime(const UWorld& World) const
	{
		if (const UWorldStorageSubsystem* Storage = WorldStorage.Get())
		{
			return Storage->GetWorldSimulationTimeMilliseconds();
		}
		return FMath::Max<int64>(0, FMath::RoundToInt64(World.GetTimeSeconds() * 1000.0));
	}

	uint32 RegistryId = AllocateRegistryId();
	uint64 NextSequence = 1;
	TArray<FSlot> Slots;
	TArray<int32> FreeSlots;
	TArray<FCharacterSnapshotHandle> Handles;
	TArray<FCharacterQuerySnapshot> Snapshots;
	TArray<FResolver> Resolvers;
	/** Remove 批次同步广播期间保留解析能力；广播返回后立即清除，Handle 仍已失效。 */
	TMap<FCharacterSnapshotHandle, FResolver> RetiringResolvers;
	TMap<TWeakObjectPtr<ACharacter>, FCharacterSnapshotHandle> ByCharacter;
	TWeakObjectPtr<UWorldStorageSubsystem> WorldStorage;
	FDelegateHandle PostActorTickHandle;
	uint64 LastPostActorFrame = MAX_uint64;
	FCharacterQuerySnapshotStats Stats;
};

UCharacterQuerySnapshotSubsystem::UCharacterQuerySnapshotSubsystem() = default;
UCharacterQuerySnapshotSubsystem::~UCharacterQuerySnapshotSubsystem() = default;

void UCharacterQuerySnapshotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldStorageSubsystem>();
	Runtime = MakePimpl<FCharacterQuerySnapshotRuntime>();
	Runtime->WorldStorage = GetWorldRef().GetSubsystem<UWorldStorageSubsystem>();
	Runtime->PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UCharacterQuerySnapshotSubsystem::HandleWorldPostActorTick);
}

void UCharacterQuerySnapshotSubsystem::Deinitialize()
{
	if (Runtime && Runtime->PostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(Runtime->PostActorTickHandle);
		Runtime->PostActorTickHandle.Reset();
	}
	Runtime.Reset();
	Super::Deinitialize();
}

FCharacterSnapshotHandle UCharacterQuerySnapshotSubsystem::RegisterCharacter(
	ACharacter& Character,
	UAbilitySystemComponent& AbilitySystem)
{
	check(IsInGameThread());
	if (!Runtime || Character.GetWorld() != GetWorld() || AbilitySystem.GetWorld() != GetWorld()
		|| AbilitySystem.GetAvatarActor() != &Character) return {};
	const TWeakObjectPtr<ACharacter> CharacterKey(&Character);
	if (const FCharacterSnapshotHandle* Existing = Runtime->ByCharacter.Find(CharacterKey))
	{
		if (Runtime->IsAlive(*Existing)
			&& ResolveCharacter(*Existing) == &Character
			&& ResolveAbilitySystem(*Existing) == &AbilitySystem) return *Existing;
		Runtime->ByCharacter.Remove(CharacterKey);
	}
	FCharacterCapsuleSnapshot Capsule;
	++Runtime->Stats.TotalCapsuleSampleCount;
	if (!CaptureCapsule(Character, Capsule)) return {};
	const int32 Slot = Runtime->FreeSlots.IsEmpty()
		? Runtime->Slots.AddDefaulted() : Runtime->FreeSlots.Pop(EAllowShrinking::No);
	FCharacterQuerySnapshotRuntime::FSlot& SlotRecord = Runtime->Slots[Slot];
	check(!SlotRecord.bAlive);
	const FCharacterSnapshotHandle Handle(Runtime->RegistryId, Slot, SlotRecord.Generation);
	const int32 Dense = Runtime->Snapshots.Num();
	SlotRecord.bAlive = true;
	SlotRecord.DenseIndex = Dense;
	Runtime->Handles.Add(Handle);
	FCharacterQuerySnapshot& Snapshot = Runtime->Snapshots.AddDefaulted_GetRef();
	Snapshot.Handle = Handle;
	Snapshot.Revision = 1;
	Snapshot.EffectiveTimeMilliseconds = Runtime->GetEffectiveTime(GetWorldRef());
	Snapshot.WorldTransform = Character.GetActorTransform();
	Snapshot.Capsule = Capsule;
	Runtime->Resolvers.Add({&Character, &AbilitySystem});
	Runtime->ByCharacter.Add(CharacterKey, Handle);
	FCharacterQuerySnapshotBatch Batch;
	Batch.Sequence = Runtime->NextSequence;
	Runtime->NextSequence = AdvanceRevision(Runtime->NextSequence);
	FCharacterQuerySnapshotChange& Change = Batch.Changes.AddDefaulted_GetRef();
	Change.Kind = ECharacterQuerySnapshotChangeKind::Upsert;
	Change.Handle = Handle;
	Change.Current = Snapshot;
	check(Batch.IsValid());
	SnapshotsCommittedEvent.Broadcast(Batch);
	++Runtime->Stats.PublishedBatchCount;
	Runtime->Stats.SnapshotCount = Runtime->Snapshots.Num();
	return Handle;
}

bool UCharacterQuerySnapshotSubsystem::UnregisterCharacter(const FCharacterSnapshotHandle Handle)
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->IsAlive(Handle)) return false;
	FCharacterQuerySnapshotRuntime::FSlot& Slot = Runtime->Slots[Handle.GetSlot()];
	const int32 Dense = Slot.DenseIndex;
	const FCharacterQuerySnapshot Previous = Runtime->Snapshots[Dense];
	const FCharacterQuerySnapshotRuntime::FResolver RetiringResolver = Runtime->Resolvers[Dense];
	Runtime->ByCharacter.Remove(Runtime->Resolvers[Dense].Character);
	const int32 Last = Runtime->Snapshots.Num() - 1;
	if (Dense != Last)
	{
		Runtime->Snapshots[Dense] = MoveTemp(Runtime->Snapshots[Last]);
		Runtime->Handles[Dense] = Runtime->Handles[Last];
		Runtime->Resolvers[Dense] = MoveTemp(Runtime->Resolvers[Last]);
		Runtime->Slots[Runtime->Handles[Dense].GetSlot()].DenseIndex = Dense;
	}
	Runtime->Snapshots.Pop(EAllowShrinking::No);
	Runtime->Handles.Pop(EAllowShrinking::No);
	Runtime->Resolvers.Pop(EAllowShrinking::No);
	Slot.bAlive = false;
	Slot.DenseIndex = INDEX_NONE;
	Slot.Generation = AdvanceGeneration(Slot.Generation);
	Runtime->FreeSlots.Add(Handle.GetSlot());
	Runtime->RetiringResolvers.Add(Handle, RetiringResolver);
	FCharacterQuerySnapshotBatch Batch;
	Batch.Sequence = Runtime->NextSequence;
	Runtime->NextSequence = AdvanceRevision(Runtime->NextSequence);
	FCharacterQuerySnapshotChange& Change = Batch.Changes.AddDefaulted_GetRef();
	Change.Kind = ECharacterQuerySnapshotChangeKind::Remove;
	Change.Handle = Handle;
	Change.Previous = Previous;
	check(Batch.IsValid());
	SnapshotsCommittedEvent.Broadcast(Batch);
	Runtime->RetiringResolvers.Remove(Handle);
	++Runtime->Stats.PublishedBatchCount;
	Runtime->Stats.SnapshotCount = Runtime->Snapshots.Num();
	return true;
}

FCharacterSnapshotHandle UCharacterQuerySnapshotSubsystem::FindSnapshot(const ACharacter& Character) const
{
	check(IsInGameThread());
	const FCharacterSnapshotHandle* Handle = Runtime
		? Runtime->ByCharacter.Find(TWeakObjectPtr<ACharacter>(const_cast<ACharacter*>(&Character))) : nullptr;
	return Handle && Runtime->IsAlive(*Handle) ? *Handle : FCharacterSnapshotHandle();
}

ACharacter* UCharacterQuerySnapshotSubsystem::ResolveCharacter(const FCharacterSnapshotHandle Handle) const
{
	check(IsInGameThread());
	if (!Runtime) return nullptr;
	ACharacter* Character = nullptr;
	if (Runtime->IsAlive(Handle))
	{
		Character = Runtime->Resolvers[Runtime->Slots[Handle.GetSlot()].DenseIndex].Character.Get();
	}
	else if (const FCharacterQuerySnapshotRuntime::FResolver* Retiring =
		Runtime->RetiringResolvers.Find(Handle))
	{
		Character = Retiring->Character.Get();
	}
	return IsValid(Character) && Character->GetWorld() == GetWorld() ? Character : nullptr;
}

UAbilitySystemComponent* UCharacterQuerySnapshotSubsystem::ResolveAbilitySystem(
	const FCharacterSnapshotHandle Handle) const
{
	check(IsInGameThread());
	if (!Runtime) return nullptr;
	UAbilitySystemComponent* Ability = nullptr;
	if (Runtime->IsAlive(Handle))
	{
		Ability = Runtime->Resolvers[
			Runtime->Slots[Handle.GetSlot()].DenseIndex].AbilitySystem.Get();
	}
	else if (const FCharacterQuerySnapshotRuntime::FResolver* Retiring =
		Runtime->RetiringResolvers.Find(Handle))
	{
		Ability = Retiring->AbilitySystem.Get();
	}
	return IsValid(Ability) && Ability->GetWorld() == GetWorld() ? Ability : nullptr;
}

bool UCharacterQuerySnapshotSubsystem::CopySnapshot(
	const FCharacterSnapshotHandle Handle,
	FCharacterQuerySnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	if (!Runtime || !Runtime->IsAlive(Handle)) return false;
	OutSnapshot = Runtime->Snapshots[Runtime->Slots[Handle.GetSlot()].DenseIndex];
	return true;
}

void UCharacterQuerySnapshotSubsystem::CopyAllSnapshots(
	TArray<FCharacterQuerySnapshot>& OutSnapshots) const
{
	check(IsInGameThread());
	OutSnapshots = Runtime ? Runtime->Snapshots : TArray<FCharacterQuerySnapshot>();
}

void UCharacterQuerySnapshotSubsystem::EnsurePostActorSnapshotsCurrent()
{
	check(IsInGameThread());
	if (!Runtime || Runtime->LastPostActorFrame == GFrameCounter) return;
	TRACE_CPUPROFILER_EVENT_SCOPE(Characters_QuerySnapshotPostActorSync);
	Runtime->LastPostActorFrame = GFrameCounter;
	Runtime->Stats.LastPostActorSampleCount = 0;
	Runtime->Stats.LastPublishedChangeCount = 0;
	++Runtime->Stats.TotalPostActorPassCount;
	FCharacterQuerySnapshotBatch Batch;
	Batch.Sequence = Runtime->NextSequence;
	TArray<FCharacterSnapshotHandle, TInlineAllocator<8>> Invalid;
	const int64 EffectiveTime = Runtime->GetEffectiveTime(GetWorldRef());
	for (int32 Index = 0; Index < Runtime->Snapshots.Num(); ++Index)
	{
		ACharacter* Character = Runtime->Resolvers[Index].Character.Get();
		UAbilitySystemComponent* Ability = Runtime->Resolvers[Index].AbilitySystem.Get();
		if (!IsValid(Character) || !IsValid(Ability) || Ability->GetAvatarActor() != Character)
		{
			Invalid.Add(Runtime->Handles[Index]);
			continue;
		}
		FCharacterCapsuleSnapshot Capsule;
		++Runtime->Stats.LastPostActorSampleCount;
		++Runtime->Stats.TotalCapsuleSampleCount;
		if (!CaptureCapsule(*Character, Capsule))
		{
			Invalid.Add(Runtime->Handles[Index]);
			continue;
		}
		FCharacterQuerySnapshot& Current = Runtime->Snapshots[Index];
		const FTransform Transform = Character->GetActorTransform();
		if (Current.WorldTransform.Equals(Transform, 0.01) && Current.Capsule.Equals(Capsule, 0.01)) continue;
		const FCharacterQuerySnapshot Previous = Current;
		Current.Revision = AdvanceRevision(Current.Revision);
		// Motion 时间窗契约要求非递减。WorldStorage 本应提供单调 Authority 时间；这里仍在
		// 中性快照边界夹住异常的迟到时间，避免一个外部时钟错误直接击穿整个服务器。
		Current.EffectiveTimeMilliseconds = FMath::Max(EffectiveTime, Previous.EffectiveTimeMilliseconds);
		Current.WorldTransform = Transform;
		Current.Capsule = Capsule;
		FCharacterQuerySnapshotChange& Change = Batch.Changes.AddDefaulted_GetRef();
		Change.Kind = ECharacterQuerySnapshotChangeKind::Motion;
		Change.Handle = Current.Handle;
		Change.Previous = Previous;
		Change.Current = Current;
	}
	if (!Batch.Changes.IsEmpty())
	{
		check(Batch.IsValid());
		Runtime->NextSequence = AdvanceRevision(Runtime->NextSequence);
		Runtime->Stats.LastPublishedChangeCount = Batch.Changes.Num();
		++Runtime->Stats.PublishedBatchCount;
		SnapshotsCommittedEvent.Broadcast(Batch);
	}
	for (const FCharacterSnapshotHandle Handle : Invalid) UnregisterCharacter(Handle);
}

FCharacterQuerySnapshotStats UCharacterQuerySnapshotSubsystem::GetStats() const
{
	check(IsInGameThread());
	FCharacterQuerySnapshotStats Result = Runtime ? Runtime->Stats : FCharacterQuerySnapshotStats();
	if (Runtime)
	{
		Result.SnapshotCount = Runtime->Snapshots.Num();
		Result.SnapshotAllocatedBytes = Runtime->Snapshots.GetAllocatedSize()
			+ Runtime->Handles.GetAllocatedSize() + Runtime->Resolvers.GetAllocatedSize()
			+ Runtime->Slots.GetAllocatedSize() + Runtime->FreeSlots.GetAllocatedSize();
	}
	return Result;
}

void UCharacterQuerySnapshotSubsystem::HandleWorldPostActorTick(
	UWorld* World,
	const ELevelTick TickType,
	const float DeltaSeconds)
{
	(void)TickType;
	(void)DeltaSeconds;
	if (Runtime && World == GetWorld()) EnsurePostActorSnapshotsCurrent();
}

bool UCharacterQuerySnapshotSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
