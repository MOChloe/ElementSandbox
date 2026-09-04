#include "Collision/BuildCollisionHost.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "ElementSandboxBuilding.h"
#include "Engine/StaticMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	uint32 GNextBuildCollisionHostId = 0;

	uint32 AllocateBuildCollisionHostId()
	{
		check(IsInGameThread());
		++GNextBuildCollisionHostId;
		if (GNextBuildCollisionHostId == 0)
		{
			++GNextBuildCollisionHostId;
		}
		return GNextBuildCollisionHostId;
	}

	void AdvanceCollisionInstanceGeneration(uint32& Generation)
	{
		++Generation;
		if (Generation == 0)
		{
			++Generation;
		}
	}

	struct FIndexedCollisionUpdate final
	{
		FBuildCollisionInstanceHandle Instance;
		FTransform WorldTransform = FTransform::Identity;
		int32 InstanceIndex = INDEX_NONE;
	};

	struct FCollisionRemoval final
	{
		FBuildCollisionInstanceHandle Instance;
		int32 InstanceIndex = INDEX_NONE;
	};

	struct FCollisionRemovalBatch final
	{
		TArray<FCollisionRemoval> Removals;
	};
}

class FBuildCollisionHostCluster final
{
public:
	UInstancedStaticMeshComponent* Component = nullptr;
	TArray<FBuildCollisionInstanceHandle> HandlesByInstanceIndex;
};

class FBuildCollisionHostData final
{
public:
	struct FInstanceSlot final
	{
		FBuildCollisionClusterKey ClusterKey;
		uint32 Generation = 1;
		int32 InstanceIndex = INDEX_NONE;
		int32 NextFreeIndex = INDEX_NONE;
		bool bAlive = false;
	};

	FBuildCollisionHostData()
		: HostId(AllocateBuildCollisionHostId())
	{
	}

	FInstanceSlot* FindSlot(const FBuildCollisionInstanceHandle Handle)
	{
		if (!Handle.IsSet()
			|| Handle.GetHostId() != HostId
			|| !Slots.IsValidIndex(Handle.GetIndex()))
		{
			return nullptr;
		}

		FInstanceSlot& Slot = Slots[Handle.GetIndex()];
		return Slot.bAlive && Slot.Generation == Handle.GetGeneration()
			? &Slot
			: nullptr;
	}

	const FInstanceSlot* FindSlot(const FBuildCollisionInstanceHandle Handle) const
	{
		return const_cast<FBuildCollisionHostData*>(this)->FindSlot(Handle);
	}

	uint32 HostId = 0;
	TArray<FInstanceSlot> Slots;
	int32 FirstFreeIndex = INDEX_NONE;
	TMap<FBuildCollisionClusterKey, FBuildCollisionHostCluster> Clusters;
};

ABuildCollisionHost::ABuildCollisionHost()
	: Data(MakePimpl<FBuildCollisionHostData>())
{
	PrimaryActorTick.bCanEverTick = false;
	SetReplicates(false);
	SetActorHiddenInGame(true);

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// Static Collision Cluster 必须能真正挂到 Host 层级；Movable Root 会让 UE
	// 拒绝 Static ISM 的 Attach，留下未附着但仍注册的物理组件。
	SceneRoot->SetMobility(EComponentMobility::Static);
	SceneRoot->SetVisibility(false, true);
	SceneRoot->SetHiddenInGame(true, true);
	SetRootComponent(SceneRoot);
}

ABuildCollisionHost::~ABuildCollisionHost() = default;

bool ABuildCollisionHost::AddInstances(
	const FBuildCollisionClusterKey& ClusterKey,
	const TConstArrayView<FTransform> WorldTransforms,
	TArray<FBuildCollisionInstanceHandle>& OutInstances)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Collision_ApplyAdd);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CollisionApplyAdd);
	OutInstances.Reset();
	if (!Data || !ClusterKey.IsSet())
	{
		return false;
	}
	if (WorldTransforms.IsEmpty())
	{
		return true;
	}

	TArray<FTransform> ValidTransforms;
	ValidTransforms.Reserve(WorldTransforms.Num());
	for (const FTransform& Transform : WorldTransforms)
	{
		if (Transform.ContainsNaN())
		{
			return false;
		}
		ValidTransforms.Add(Transform);
	}

	FBuildCollisionHostCluster* Cluster = FindOrAddCluster(ClusterKey);
	if (!Cluster || !Cluster->Component
		|| Cluster->Component->GetInstanceCount()
			!= Cluster->HandlesByInstanceIndex.Num())
	{
		return false;
	}

	const int32 FirstExpectedIndex = Cluster->HandlesByInstanceIndex.Num();
	TArray<int32> AddedIndices = Cluster->Component->AddInstances(
		ValidTransforms,
		/*bShouldReturnIndices*/ true,
		/*bWorldSpace*/ true,
		/*bUpdateNavigation*/ false);
	bool bIndicesValid = AddedIndices.Num() == ValidTransforms.Num();
	for (int32 Index = 0; bIndicesValid && Index < AddedIndices.Num(); ++Index)
	{
		bIndicesValid = AddedIndices[Index] == FirstExpectedIndex + Index;
	}
	if (!bIndicesValid)
	{
		AddedIndices.Sort(TGreater<int32>());
		if (!AddedIndices.IsEmpty())
		{
			Cluster->Component->RemoveInstances(AddedIndices, true);
		}
		DestroyClusterIfEmpty(ClusterKey);
		return false;
	}

	OutInstances.Reserve(AddedIndices.Num());
	for (const int32 InstanceIndex : AddedIndices)
	{
		const FBuildCollisionInstanceHandle Handle = AllocateInstanceHandle();
		FBuildCollisionHostData::FInstanceSlot& Slot = Data->Slots[Handle.GetIndex()];
		Slot.ClusterKey = ClusterKey;
		Slot.InstanceIndex = InstanceIndex;
		Cluster->HandlesByInstanceIndex.Add(Handle);
		OutInstances.Add(Handle);
	}
	return true;
}

bool ABuildCollisionHost::UpdateInstances(
	const FBuildCollisionClusterKey& ClusterKey,
	const TConstArrayView<FBuildCollisionInstanceUpdate> Updates)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Collision_ApplyUpdate);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CollisionApplyUpdate);
	if (!Data || !ClusterKey.IsSet())
	{
		return false;
	}
	if (Updates.IsEmpty())
	{
		return true;
	}

	FBuildCollisionHostCluster* Cluster = Data->Clusters.Find(ClusterKey);
	if (!Cluster || !Cluster->Component
		|| Cluster->Component->GetInstanceCount()
			!= Cluster->HandlesByInstanceIndex.Num())
	{
		return false;
	}

	TSet<FBuildCollisionInstanceHandle> UniqueInstances;
	TArray<FIndexedCollisionUpdate> SortedUpdates;
	UniqueInstances.Reserve(Updates.Num());
	SortedUpdates.Reserve(Updates.Num());
	for (const FBuildCollisionInstanceUpdate& Update : Updates)
	{
		const FBuildCollisionHostData::FInstanceSlot* Slot = Data->FindSlot(Update.Instance);
		if (!Slot
			|| Slot->ClusterKey != ClusterKey
			|| Update.WorldTransform.ContainsNaN()
			|| UniqueInstances.Contains(Update.Instance)
			|| !Cluster->HandlesByInstanceIndex.IsValidIndex(Slot->InstanceIndex)
			|| Cluster->HandlesByInstanceIndex[Slot->InstanceIndex] != Update.Instance)
		{
			return false;
		}
		UniqueInstances.Add(Update.Instance);
		SortedUpdates.Add({Update.Instance, Update.WorldTransform, Slot->InstanceIndex});
	}

	SortedUpdates.Sort(
		[](const FIndexedCollisionUpdate& Left, const FIndexedCollisionUpdate& Right)
		{
			return Left.InstanceIndex < Right.InstanceIndex;
		});
	const bool bTeleport = ClusterKey.Mobility != EBuildCollisionMobility::Kinematic;
	for (int32 RunStart = 0; RunStart < SortedUpdates.Num();)
	{
		int32 RunEnd = RunStart + 1;
		while (RunEnd < SortedUpdates.Num()
			&& SortedUpdates[RunEnd].InstanceIndex
				== SortedUpdates[RunEnd - 1].InstanceIndex + 1)
		{
			++RunEnd;
		}

		TArray<FTransform, TInlineAllocator<8>> RunTransforms;
		RunTransforms.Reserve(RunEnd - RunStart);
		for (int32 Index = RunStart; Index < RunEnd; ++Index)
		{
			RunTransforms.Add(SortedUpdates[Index].WorldTransform);
		}

		if (!Cluster->Component->BatchUpdateInstancesTransforms(
			SortedUpdates[RunStart].InstanceIndex,
			MakeArrayView(RunTransforms),
			/*bWorldSpace*/ true,
			/*bMarkRenderStateDirty*/ false,
			bTeleport))
		{
			return false;
		}
		RunStart = RunEnd;
	}

	return true;
}

bool ABuildCollisionHost::RemoveInstances(
	const TConstArrayView<FBuildCollisionInstanceHandle> Instances)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Collision_ApplyRemove);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, CollisionApplyRemove);
	if (!Data)
	{
		return false;
	}
	if (Instances.IsEmpty())
	{
		return true;
	}

	TSet<FBuildCollisionInstanceHandle> UniqueInstances;
	TMap<FBuildCollisionClusterKey, FCollisionRemovalBatch> Batches;
	UniqueInstances.Reserve(Instances.Num());
	for (const FBuildCollisionInstanceHandle Instance : Instances)
	{
		const FBuildCollisionHostData::FInstanceSlot* Slot = Data->FindSlot(Instance);
		const FBuildCollisionHostCluster* Cluster = Slot
			? Data->Clusters.Find(Slot->ClusterKey)
			: nullptr;
		if (!Slot || !Cluster || !Cluster->Component
			|| UniqueInstances.Contains(Instance)
			|| !Cluster->HandlesByInstanceIndex.IsValidIndex(Slot->InstanceIndex)
			|| Cluster->HandlesByInstanceIndex[Slot->InstanceIndex] != Instance)
		{
			return false;
		}

		UniqueInstances.Add(Instance);
		Batches.FindOrAdd(Slot->ClusterKey).Removals.Add(
			{Instance, Slot->InstanceIndex});
	}

	for (TPair<FBuildCollisionClusterKey, FCollisionRemovalBatch>& Pair : Batches)
	{
		FBuildCollisionHostCluster* Cluster = Data->Clusters.Find(Pair.Key);
		check(Cluster && Cluster->Component);
		Pair.Value.Removals.Sort(
			[](const FCollisionRemoval& Left, const FCollisionRemoval& Right)
			{
				return Left.InstanceIndex > Right.InstanceIndex;
			});

		TArray<int32> Indices;
		Indices.Reserve(Pair.Value.Removals.Num());
		for (const FCollisionRemoval& Removal : Pair.Value.Removals)
		{
			Indices.Add(Removal.InstanceIndex);
		}
		if (!Cluster->Component->RemoveInstances(Indices, true))
		{
			return false;
		}

		for (const FCollisionRemoval& Removal : Pair.Value.Removals)
		{
			const int32 LastIndex = Cluster->HandlesByInstanceIndex.Num() - 1;
			check(Cluster->HandlesByInstanceIndex.IsValidIndex(Removal.InstanceIndex));
			check(Cluster->HandlesByInstanceIndex[Removal.InstanceIndex]
				== Removal.Instance);
			const FBuildCollisionInstanceHandle MovedInstance =
				Cluster->HandlesByInstanceIndex[LastIndex];
			if (Removal.InstanceIndex != LastIndex)
			{
				Cluster->HandlesByInstanceIndex[Removal.InstanceIndex] = MovedInstance;
				FBuildCollisionHostData::FInstanceSlot* MovedSlot =
					Data->FindSlot(MovedInstance);
				check(MovedSlot);
				MovedSlot->InstanceIndex = Removal.InstanceIndex;
			}
			Cluster->HandlesByInstanceIndex.Pop(EAllowShrinking::No);
			ReleaseInstanceHandle(Removal.Instance.GetIndex());
		}

		DestroyClusterIfEmpty(Pair.Key);
	}
	return true;
}

bool ABuildCollisionHost::IsValidInstance(
	const FBuildCollisionInstanceHandle Instance) const
{
	check(IsInGameThread());
	return Data && Data->FindSlot(Instance) != nullptr;
}

bool ABuildCollisionHost::TryGetInstanceTransform(
	const FBuildCollisionInstanceHandle Instance,
	FTransform& OutWorldTransform) const
{
	check(IsInGameThread());
	const FBuildCollisionHostData::FInstanceSlot* Slot = Data
		? Data->FindSlot(Instance)
		: nullptr;
	const FBuildCollisionHostCluster* Cluster = Slot
		? Data->Clusters.Find(Slot->ClusterKey)
		: nullptr;
	return Cluster && Cluster->Component
		&& Cluster->Component->GetInstanceTransform(
			Slot->InstanceIndex,
			OutWorldTransform,
			/*bWorldSpace*/ true);
}

UInstancedStaticMeshComponent* ABuildCollisionHost::GetClusterComponent(
	const FBuildCollisionClusterKey& ClusterKey) const
{
	check(IsInGameThread());
	const FBuildCollisionHostCluster* Cluster = Data
		? Data->Clusters.Find(ClusterKey)
		: nullptr;
	return Cluster ? Cluster->Component : nullptr;
}

int32 ABuildCollisionHost::GetInstanceCount() const
{
	check(IsInGameThread());
	int32 InstanceCount = 0;
	if (Data)
	{
		for (const TPair<FBuildCollisionClusterKey, FBuildCollisionHostCluster>& Pair
			: Data->Clusters)
		{
			InstanceCount += Pair.Value.HandlesByInstanceIndex.Num();
		}
	}
	return InstanceCount;
}

int32 ABuildCollisionHost::GetClusterCount() const
{
	check(IsInGameThread());
	return Data ? Data->Clusters.Num() : 0;
}

SIZE_T ABuildCollisionHost::GetEstimatedCPUAllocatedSize() const
{
	check(IsInGameThread());
	if (!Data)
	{
		return 0;
	}

	SIZE_T AllocatedSize = Data->Slots.GetAllocatedSize()
		+ Data->Clusters.GetAllocatedSize();
	for (const TPair<FBuildCollisionClusterKey, FBuildCollisionHostCluster>& Pair
		: Data->Clusters)
	{
		AllocatedSize += Pair.Value.HandlesByInstanceIndex.GetAllocatedSize();
		AllocatedSize += static_cast<SIZE_T>(Pair.Value.HandlesByInstanceIndex.Num())
			* sizeof(FInstancedStaticMeshInstanceData);
	}
	return AllocatedSize;
}

void ABuildCollisionHost::ClearInstances()
{
	check(IsInGameThread());
	if (!Data)
	{
		return;
	}

	for (TPair<FBuildCollisionClusterKey, FBuildCollisionHostCluster>& Pair
		: Data->Clusters)
	{
		if (UInstancedStaticMeshComponent* Component = Pair.Value.Component)
		{
			Component->ClearInstances();
			RemoveInstanceComponent(Component);
			Component->DestroyComponent();
		}
	}
	Data->Clusters.Reset();

	Data->FirstFreeIndex = INDEX_NONE;
	for (int32 Index = Data->Slots.Num() - 1; Index >= 0; --Index)
	{
		FBuildCollisionHostData::FInstanceSlot& Slot = Data->Slots[Index];
		if (Slot.bAlive)
		{
			AdvanceCollisionInstanceGeneration(Slot.Generation);
		}
		Slot.bAlive = false;
		Slot.ClusterKey = {};
		Slot.InstanceIndex = INDEX_NONE;
		Slot.NextFreeIndex = Data->FirstFreeIndex;
		Data->FirstFreeIndex = Index;
	}
}

FBuildCollisionHostCluster* ABuildCollisionHost::FindOrAddCluster(
	const FBuildCollisionClusterKey& ClusterKey)
{
	if (FBuildCollisionHostCluster* Existing = Data->Clusters.Find(ClusterKey))
	{
		return Existing;
	}

	UInstancedStaticMeshComponent* Component =
		NewObject<UInstancedStaticMeshComponent>(
			this,
			MakeUniqueObjectName(
				this,
				UInstancedStaticMeshComponent::StaticClass(),
				TEXT("BuildCollisionISM")));
	if (!Component)
	{
		return nullptr;
	}

	Component->SetupAttachment(GetRootComponent());
	Component->SetRemoveSwap();
	Component->SetMobility(
		ClusterKey.Mobility == EBuildCollisionMobility::Kinematic
			? EComponentMobility::Movable
			: EComponentMobility::Static);
	Component->SetStaticMesh(ClusterKey.Mesh);
	Component->SetCollisionProfileName(ClusterKey.CollisionProfileName);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(false);
		Component->bCastDynamicShadow = false;
		Component->bCastStaticShadow = false;
		Component->SetVisibility(false, true);
	Component->SetHiddenInGame(true, true);
	AddInstanceComponent(Component);
	Component->RegisterComponent();

	FBuildCollisionHostCluster NewCluster;
	NewCluster.Component = Component;
	Data->Clusters.Add(ClusterKey, MoveTemp(NewCluster));
	return Data->Clusters.Find(ClusterKey);
}

FBuildCollisionInstanceHandle ABuildCollisionHost::AllocateInstanceHandle()
{
	int32 SlotIndex = INDEX_NONE;
	if (Data->FirstFreeIndex != INDEX_NONE)
	{
		SlotIndex = Data->FirstFreeIndex;
		FBuildCollisionHostData::FInstanceSlot& Slot = Data->Slots[SlotIndex];
		Data->FirstFreeIndex = Slot.NextFreeIndex;
		Slot.NextFreeIndex = INDEX_NONE;
	}
	else
	{
		SlotIndex = Data->Slots.AddDefaulted();
	}

	FBuildCollisionHostData::FInstanceSlot& Slot = Data->Slots[SlotIndex];
	check(!Slot.bAlive);
	check(Slot.Generation != 0);
	Slot.bAlive = true;
	return FBuildCollisionInstanceHandle(Data->HostId, SlotIndex, Slot.Generation);
}

void ABuildCollisionHost::ReleaseInstanceHandle(const int32 SlotIndex)
{
	check(Data->Slots.IsValidIndex(SlotIndex));
	FBuildCollisionHostData::FInstanceSlot& Slot = Data->Slots[SlotIndex];
	check(Slot.bAlive);
	Slot.bAlive = false;
	AdvanceCollisionInstanceGeneration(Slot.Generation);
	Slot.ClusterKey = {};
	Slot.InstanceIndex = INDEX_NONE;
	Slot.NextFreeIndex = Data->FirstFreeIndex;
	Data->FirstFreeIndex = SlotIndex;
}

void ABuildCollisionHost::DestroyClusterIfEmpty(
	const FBuildCollisionClusterKey& ClusterKey)
{
	FBuildCollisionHostCluster* Cluster = Data->Clusters.Find(ClusterKey);
	if (!Cluster || !Cluster->HandlesByInstanceIndex.IsEmpty())
	{
		return;
	}

	if (UInstancedStaticMeshComponent* Component = Cluster->Component)
	{
		RemoveInstanceComponent(Component);
		Component->DestroyComponent();
	}
	Data->Clusters.Remove(ClusterKey);
}
