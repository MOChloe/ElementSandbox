#include "MeshPoolRenderHost.h"

#include "ElementSandboxPresentation.h"
#include "Rendering/MeshPoolHierarchicalInstancedStaticMeshComponent.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "UObject/UObjectGlobals.h"

namespace
{
struct FPhysicalLocation final
{
	FMeshPoolClusterKey Cluster;
	int32 InstanceIndex = INDEX_NONE;
};

struct FMeshPoolHostCluster final
{
	UInstancedStaticMeshComponent* Component = nullptr;
	TArray<FMeshPoolInstanceHandle> HandlesByInstanceIndex;
};

struct FIndexedUpdate final
{
	FMeshPoolInstanceUpdate Update;
	int32 InstanceIndex = INDEX_NONE;
};

struct FIndexedCustomUpdate final
{
	FMeshPoolCustomDataUpdate Update;
	int32 InstanceIndex = INDEX_NONE;
};

struct FRemoval final
{
	FMeshPoolInstanceHandle Instance;
	int32 InstanceIndex = INDEX_NONE;
};

struct FRemovalBatch final
{
	TArray<FRemoval> Items;
};

bool IsFiniteCustomData(const TConstArrayView<float> Values)
{
	for (const float Value : Values)
	{
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
	}
	return true;
}

bool SetCustomDataRange(UInstancedStaticMeshComponent& Component, const int32 InstanceIndexStart,
	const int32 InstanceIndexEnd, const TConstArrayView<float> CustomDataFloats)
{
	if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
			Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(&Component))
	{
		return Hierarchical->SetMeshPoolCustomDataRange(
			InstanceIndexStart, InstanceIndexEnd, CustomDataFloats);
	}
	return Component.SetCustomData(
		InstanceIndexStart, InstanceIndexEnd, CustomDataFloats, /*bMarkRenderStateDirty*/ false);
}
} // namespace

class FMeshPoolRenderHostData final
{
public:
	TMap<FMeshPoolClusterKey, FMeshPoolHostCluster> Clusters;
	TMap<FMeshPoolInstanceHandle, FPhysicalLocation> Locations;
	TSet<UInstancedStaticMeshComponent*> DirtyComponents;
	TSet<FMeshPoolClusterKey> PendingDestroyClusters;
	int32 BulkEditDepth = 0;
	uint64 RetiredTreeBuildRequests = 0;
	uint64 RetiredTreeBuildRetries = 0;
	uint64 RetiredDeferredTreeBuildRequests = 0;
	uint64 RetiredCoalescedTreeBuildRequests = 0;
};

namespace
{
void AccumulateRetiredTreeBuildStats(FMeshPoolRenderHostData& Data,
									 const UMeshPoolHierarchicalInstancedStaticMeshComponent& Component)
{
	Data.RetiredTreeBuildRequests += Component.GetTreeBuildRequestCount();
	Data.RetiredTreeBuildRetries += Component.GetTreeBuildRetryCount();
	Data.RetiredDeferredTreeBuildRequests += Component.GetDeferredTreeBuildRequestCount();
	Data.RetiredCoalescedTreeBuildRequests += Component.GetCoalescedTreeBuildRequestCount();
}

FMeshPoolHostCluster* FindOrAddCluster(AMeshPoolRenderHost& Host, FMeshPoolRenderHostData& Data,
									   const FMeshPoolClusterKey& Key)
{
	if (FMeshPoolHostCluster* Existing = Data.Clusters.Find(Key))
	{
		Data.PendingDestroyClusters.Remove(Key);
		return Existing;
	}
	if (!Key.IsSet() ||
		(Key.MaterialOverride && !Key.MaterialOverride->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes)))
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* Component = nullptr;
	if (Key.Backend == EMeshPoolBackend::ImmediateMovable)
	{
		Component = NewObject<UInstancedStaticMeshComponent>(
			&Host, MakeUniqueObjectName(&Host, UInstancedStaticMeshComponent::StaticClass(), TEXT("MeshPoolHotISM")));
		if (Component)
		{
			Component->SetRemoveSwap();
			Component->SetMobility(EComponentMobility::Movable);
		}
	}
	else
	{
		Component = NewObject<UMeshPoolHierarchicalInstancedStaticMeshComponent>(
			&Host, MakeUniqueObjectName(&Host, UMeshPoolHierarchicalInstancedStaticMeshComponent::StaticClass(),
										TEXT("MeshPoolColdHISM")));
	}
	if (!Component)
	{
		return nullptr;
	}

	Component->SetupAttachment(Host.GetRootComponent());
	Component->SetStaticMesh(Key.Mesh);
	Component->SetNumCustomDataFloats(Key.CustomDataFloatCount);
	if (Key.MaterialOverride)
	{
		Component->SetMaterial(0, Key.MaterialOverride);
	}
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bCastDynamicShadow = false;
	Component->bCastStaticShadow = false;
	Component->SetReceivesDecals(false);
	Host.AddInstanceComponent(Component);
	Component->RegisterComponent();

	if (Data.BulkEditDepth > 0)
	{
		if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Component))
		{
			Hierarchical->BeginBulkEdit();
		}
	}

	FMeshPoolHostCluster NewCluster;
	NewCluster.Component = Component;
	Data.Clusters.Add(Key, MoveTemp(NewCluster));
	return Data.Clusters.Find(Key);
}

bool ApplyCustomDataRuns(UInstancedStaticMeshComponent& Component, const int32 CustomDataFloatCount,
						 TArray<FIndexedCustomUpdate>& SortedUpdates)
{
	if (CustomDataFloatCount <= 0 || SortedUpdates.IsEmpty())
	{
		return SortedUpdates.IsEmpty();
	}
	SortedUpdates.Sort([](const FIndexedCustomUpdate& Left, const FIndexedCustomUpdate& Right)
					   { return Left.InstanceIndex < Right.InstanceIndex; });

	for (int32 RunStart = 0; RunStart < SortedUpdates.Num();)
	{
		int32 RunEnd = RunStart + 1;
		while (RunEnd < SortedUpdates.Num() &&
			   SortedUpdates[RunEnd].InstanceIndex == SortedUpdates[RunEnd - 1].InstanceIndex + 1)
		{
			++RunEnd;
		}
		TArray<float> Flattened;
		Flattened.Reserve((RunEnd - RunStart) * CustomDataFloatCount);
		for (int32 Index = RunStart; Index < RunEnd; ++Index)
		{
			Flattened.Append(SortedUpdates[Index].Update.CustomData);
		}
		if (!SetCustomDataRange(Component, SortedUpdates[RunStart].InstanceIndex,
								SortedUpdates[RunEnd - 1].InstanceIndex, Flattened))
		{
			return false;
		}
		RunStart = RunEnd;
	}
	return true;
}
} // namespace

AMeshPoolRenderHost::AMeshPoolRenderHost() : Data(MakePimpl<FMeshPoolRenderHostData>())
{
	PrimaryActorTick.bCanEverTick = false;
	SetReplicates(false);
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);
}

AMeshPoolRenderHost::~AMeshPoolRenderHost() = default;

void AMeshPoolRenderHost::BeginBulkEdit()
{
	check(IsInGameThread());
	check(Data);
	if (Data->BulkEditDepth++ > 0)
	{
		return;
	}
	Data->DirtyComponents.Reset();
	for (TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			Hierarchical->BeginBulkEdit();
		}
	}
}

void AMeshPoolRenderHost::EndBulkEdit(
	const double CurrentTimeSeconds,
	const int32 SynchronousTreeBuildMaxInstances)
{
	check(IsInGameThread());
	check(Data && Data->BulkEditDepth > 0);
	if (--Data->BulkEditDepth > 0)
	{
		return;
	}

	for (TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			if (Data->PendingDestroyClusters.Contains(Pair.Key))
			{
				Hierarchical->CancelBulkEdit();
			}
			else
			{
				Hierarchical->EndBulkEdit(CurrentTimeSeconds, Data->DirtyComponents.Contains(Pair.Value.Component));
			}
		}
	}
	for (UInstancedStaticMeshComponent* Component : Data->DirtyComponents)
	{
		if (IsValid(Component))
		{
			// 批量 Update/CustomData 使用 bMarkRenderStateDirty=false，必须在批尾提交实例脏标记。
			// HISM 追加区间另由专用组件标记 Render State Dirty；缓冲更新无法替代其绘制范围刷新。
			Component->MarkRenderInstancesDirty();
		}
	}
	for (TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (!Data->PendingDestroyClusters.Contains(Pair.Key)
			&& Data->DirtyComponents.Contains(Pair.Value.Component))
		{
			if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
					Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
			{
				Hierarchical->PublishSmallTreeImmediately(SynchronousTreeBuildMaxInstances);
			}
		}
	}
	Data->DirtyComponents.Reset();

	TArray<FMeshPoolClusterKey> DestroyKeys = Data->PendingDestroyClusters.Array();
	Data->PendingDestroyClusters.Reset();
	for (const FMeshPoolClusterKey& Key : DestroyKeys)
	{
		DestroyClusterIfEmpty(Key);
	}
}

void AMeshPoolRenderHost::ProcessDeferredTreeBuilds(const double CurrentTimeSeconds, const double QuietSeconds,
											const double MaxDeferralSeconds, const bool bForce)
{
	check(IsInGameThread());
	if (!Data || Data->BulkEditDepth > 0)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_HISM_TreeSchedule);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, HISMTreeSchedule);

	// UE 在销毁正在 BuildTreeAsync 的 HISM 时会等待其 TaskGraph 任务。空 Cluster 先保留到
	// 构建自然完成，避免批量源删除把后台树构建重新同步成 GameThread 尖峰。
	TArray<FMeshPoolClusterKey> RetiringKeys = Data->PendingDestroyClusters.Array();
	Data->PendingDestroyClusters.Reset();
	for (const FMeshPoolClusterKey& Key : RetiringKeys)
	{
		DestroyClusterIfEmpty(Key);
	}

	// 全局 Host 同时只允许一个 HISM Tree Build。多个大 Cluster 并发完成会在同一帧把
	// ApplyBuildTreeAsync 和销毁依赖集中压回 GameThread。
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
			Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component);
		if (Hierarchical && Hierarchical->IsAsyncBuilding())
		{
			return;
		}
	}
	for (TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			if (Hierarchical->TryStartDeferredTreeBuild(
				CurrentTimeSeconds, QuietSeconds, MaxDeferralSeconds, bForce))
			{
				break;
			}
		}
	}
}

bool AMeshPoolRenderHost::HasDeferredTreeBuilds() const
{
	if (!Data)
	{
		return false;
	}
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
			Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component);
		if (Hierarchical && (Hierarchical->HasDeferredTreeBuild() || Hierarchical->IsAsyncBuilding()))
		{
			return true;
		}
	}
	return !Data->PendingDestroyClusters.IsEmpty();
}

bool AMeshPoolRenderHost::AddInstances(const FMeshPoolClusterKey& Key,
									   const TConstArrayView<FMeshPoolInstanceHandle> Instances,
									   const TConstArrayView<FTransform> WorldTransforms,
									   const TConstArrayView<float> FlattenedCustomData)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_Add);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolAdd);
	if (!Data || !Key.IsSet() || Instances.Num() != WorldTransforms.Num())
	{
		return false;
	}
	if (Instances.IsEmpty())
	{
		return true;
	}
	if ((Key.CustomDataFloatCount == 0 && !FlattenedCustomData.IsEmpty()) ||
		(Key.CustomDataFloatCount > 0 && FlattenedCustomData.Num() != Instances.Num() * Key.CustomDataFloatCount) ||
		!IsFiniteCustomData(FlattenedCustomData))
	{
		return false;
	}
	for (int32 Index = 0; Index < Instances.Num(); ++Index)
	{
		if (!Instances[Index].IsSet() || Data->Locations.Contains(Instances[Index]) ||
			WorldTransforms[Index].ContainsNaN())
		{
			return false;
		}
	}

	FMeshPoolHostCluster* Cluster = FindOrAddCluster(*this, *Data, Key);
	if (!Cluster || !Cluster->Component ||
		Cluster->Component->GetInstanceCount() != Cluster->HandlesByInstanceIndex.Num())
	{
		return false;
	}
	Cluster->Component->PreAllocateInstancesMemory(WorldTransforms.Num());
	const int32 FirstIndex = Cluster->HandlesByInstanceIndex.Num();
	TArray<FTransform> Transforms;
	Transforms.Append(WorldTransforms);
	TArray<int32> AddedIndices = Cluster->Component->AddInstances(Transforms,
																  /*bShouldReturnIndices*/ true,
																  /*bWorldSpace*/ true,
																  /*bUpdateNavigation*/ false);
	bool bValidIndices = AddedIndices.Num() == Instances.Num();
	for (int32 Index = 0; bValidIndices && Index < AddedIndices.Num(); ++Index)
	{
		bValidIndices = AddedIndices[Index] == FirstIndex + Index;
	}
	if (!bValidIndices)
	{
		AddedIndices.Sort(TGreater<int32>());
		if (!AddedIndices.IsEmpty())
		{
			Cluster->Component->RemoveInstances(AddedIndices, true);
		}
		DestroyClusterIfEmpty(Key);
		return false;
	}
	if (Key.CustomDataFloatCount > 0 &&
		!SetCustomDataRange(*Cluster->Component, FirstIndex, FirstIndex + Instances.Num() - 1,
			FlattenedCustomData))
	{
		AddedIndices.Sort(TGreater<int32>());
		Cluster->Component->RemoveInstances(AddedIndices, true);
		DestroyClusterIfEmpty(Key);
		return false;
	}

	for (int32 Index = 0; Index < Instances.Num(); ++Index)
	{
		Cluster->HandlesByInstanceIndex.Add(Instances[Index]);
		Data->Locations.Add(Instances[Index], {Key, AddedIndices[Index]});
	}
	MarkClusterDirty(Key);
	return true;
}

bool AMeshPoolRenderHost::UpdateInstances(const FMeshPoolClusterKey& Key,
										  const TConstArrayView<FMeshPoolInstanceUpdate> Updates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_Update);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolUpdate);
	check(IsInGameThread());
	if (!Data || !Key.IsSet())
	{
		return false;
	}
	if (Updates.IsEmpty())
	{
		return true;
	}
	FMeshPoolHostCluster* Cluster = Data->Clusters.Find(Key);
	if (!Cluster || !Cluster->Component)
	{
		return false;
	}

	TSet<FMeshPoolInstanceHandle> Unique;
	TArray<FIndexedUpdate> Sorted;
	TArray<FIndexedCustomUpdate> CustomUpdates;
	Sorted.Reserve(Updates.Num());
	for (const FMeshPoolInstanceUpdate& Update : Updates)
	{
		const FPhysicalLocation* Location = Data->Locations.Find(Update.Instance);
		if (!Location || Location->Cluster != Key || Update.WorldTransform.ContainsNaN() ||
			Unique.Contains(Update.Instance) ||
			(!Update.CustomData.IsEmpty() &&
			 (Update.CustomData.Num() != Key.CustomDataFloatCount || !IsFiniteCustomData(Update.CustomData))))
		{
			return false;
		}
		Unique.Add(Update.Instance);
		Sorted.Add({Update, Location->InstanceIndex});
		if (!Update.CustomData.IsEmpty())
		{
			FMeshPoolCustomDataUpdate Custom;
			Custom.Instance = Update.Instance;
			Custom.CustomData = Update.CustomData;
			CustomUpdates.Add({MoveTemp(Custom), Location->InstanceIndex});
		}
	}
	Sorted.Sort([](const FIndexedUpdate& Left, const FIndexedUpdate& Right)
				{ return Left.InstanceIndex < Right.InstanceIndex; });

	UHierarchicalInstancedStaticMeshComponent* Hierarchical =
		Cast<UHierarchicalInstancedStaticMeshComponent>(Cluster->Component);
	for (int32 RunStart = 0; RunStart < Sorted.Num();)
	{
		int32 RunEnd = RunStart + 1;
		while (RunEnd < Sorted.Num() && Sorted[RunEnd].InstanceIndex == Sorted[RunEnd - 1].InstanceIndex + 1)
		{
			++RunEnd;
		}
		bool bUpdated = false;
		if (Hierarchical)
		{
			TArray<FInstancedStaticMeshInstanceData> InstanceData;
			InstanceData.SetNum(RunEnd - RunStart);
			for (int32 Index = RunStart; Index < RunEnd; ++Index)
			{
				const FTransform Local = Sorted[Index].Update.WorldTransform.GetRelativeTransform(
					Cluster->Component->GetComponentTransform());
				InstanceData[Index - RunStart].Transform = Local.ToMatrixWithScale();
			}
			bUpdated = Hierarchical->BatchUpdateInstancesData(Sorted[RunStart].InstanceIndex, InstanceData.Num(),
															  InstanceData.GetData(), false, true);
		}
		else
		{
			TArray<FTransform> Transforms;
			Transforms.Reserve(RunEnd - RunStart);
			for (int32 Index = RunStart; Index < RunEnd; ++Index)
			{
				Transforms.Add(Sorted[Index].Update.WorldTransform);
			}
			bUpdated = Cluster->Component->BatchUpdateInstancesTransforms(Sorted[RunStart].InstanceIndex, Transforms,
																		  true, false, true);
		}
		if (!bUpdated)
		{
			return false;
		}
		RunStart = RunEnd;
	}
	if (!CustomUpdates.IsEmpty() && !ApplyCustomDataRuns(*Cluster->Component, Key.CustomDataFloatCount, CustomUpdates))
	{
		return false;
	}
	MarkClusterDirty(Key);
	return true;
}

bool AMeshPoolRenderHost::UpdateCustomData(const FMeshPoolClusterKey& Key,
										   const TConstArrayView<FMeshPoolCustomDataUpdate> Updates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_CustomData);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolCustomData);
	check(IsInGameThread());
	if (!Data || !Key.IsSet() || Key.CustomDataFloatCount <= 0)
	{
		return false;
	}
	if (Updates.IsEmpty())
	{
		return true;
	}
	FMeshPoolHostCluster* Cluster = Data->Clusters.Find(Key);
	if (!Cluster || !Cluster->Component)
	{
		return false;
	}
	TSet<FMeshPoolInstanceHandle> Unique;
	TArray<FIndexedCustomUpdate> Sorted;
	Sorted.Reserve(Updates.Num());
	for (const FMeshPoolCustomDataUpdate& Update : Updates)
	{
		const FPhysicalLocation* Location = Data->Locations.Find(Update.Instance);
		if (!Location || Location->Cluster != Key || Unique.Contains(Update.Instance) ||
			Update.CustomData.Num() != Key.CustomDataFloatCount || !IsFiniteCustomData(Update.CustomData))
		{
			return false;
		}
		Unique.Add(Update.Instance);
		Sorted.Add({Update, Location->InstanceIndex});
	}
	if (!ApplyCustomDataRuns(*Cluster->Component, Key.CustomDataFloatCount, Sorted))
	{
		return false;
	}
	MarkClusterDirty(Key);
	return true;
}

bool AMeshPoolRenderHost::MigrateInstances(const FMeshPoolClusterKey& SourceKey, const FMeshPoolClusterKey& TargetKey,
											   const TConstArrayView<FMeshPoolInstanceUpdate> Updates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_Migrate);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolMigrate);
	check(IsInGameThread());
	if (!Data || !SourceKey.IsSet() || !TargetKey.IsSet() || SourceKey == TargetKey)
	{
		return false;
	}
	if (Updates.IsEmpty())
	{
		return true;
	}
	FMeshPoolHostCluster* Source = Data->Clusters.Find(SourceKey);
	if (!Source || !Source->Component)
	{
		return false;
	}

	TSet<FMeshPoolInstanceHandle> Unique;
	TArray<FRemoval> SourceRemovals;
	TArray<FTransform> TargetTransforms;
	TArray<float> TargetCustomData;
	for (const FMeshPoolInstanceUpdate& Update : Updates)
	{
		const FPhysicalLocation* Location = Data->Locations.Find(Update.Instance);
		if (!Location || Location->Cluster != SourceKey || Unique.Contains(Update.Instance) ||
			Update.WorldTransform.ContainsNaN() ||
			(!Update.CustomData.IsEmpty() &&
			 (Update.CustomData.Num() != TargetKey.CustomDataFloatCount || !IsFiniteCustomData(Update.CustomData))))
		{
			return false;
		}
		Unique.Add(Update.Instance);
		SourceRemovals.Add({Update.Instance, Location->InstanceIndex});
		TargetTransforms.Add(Update.WorldTransform);
		if (TargetKey.CustomDataFloatCount > 0)
		{
			if (Update.CustomData.IsEmpty())
			{
				TargetCustomData.AddZeroed(TargetKey.CustomDataFloatCount);
			}
			else
			{
				TargetCustomData.Append(Update.CustomData);
			}
		}
	}

	FMeshPoolHostCluster* Target = FindOrAddCluster(*this, *Data, TargetKey);
	Source = Data->Clusters.Find(SourceKey);
	Target = Data->Clusters.Find(TargetKey);
	if (!Source || !Source->Component || !Target || !Target->Component)
	{
		return false;
	}
	Target->Component->PreAllocateInstancesMemory(Updates.Num());
	const int32 FirstTargetIndex = Target->HandlesByInstanceIndex.Num();
	TArray<int32> TargetIndices = Target->Component->AddInstances(TargetTransforms, true, true, false);
	bool bTargetValid = TargetIndices.Num() == Updates.Num();
	for (int32 Index = 0; bTargetValid && Index < TargetIndices.Num(); ++Index)
	{
		bTargetValid = TargetIndices[Index] == FirstTargetIndex + Index;
	}
	if (!bTargetValid)
	{
		TargetIndices.Sort(TGreater<int32>());
		Target->Component->RemoveInstances(TargetIndices, true);
		DestroyClusterIfEmpty(TargetKey);
		return false;
	}
	if (TargetKey.CustomDataFloatCount > 0 &&
		!SetCustomDataRange(*Target->Component, FirstTargetIndex,
			FirstTargetIndex + Updates.Num() - 1, TargetCustomData))
	{
		TargetIndices.Sort(TGreater<int32>());
		Target->Component->RemoveInstances(TargetIndices, true);
		DestroyClusterIfEmpty(TargetKey);
		return false;
	}

	SourceRemovals.Sort([](const FRemoval& Left, const FRemoval& Right)
						{ return Left.InstanceIndex > Right.InstanceIndex; });
	TArray<int32> SourceIndices;
	for (const FRemoval& Removal : SourceRemovals)
	{
		SourceIndices.Add(Removal.InstanceIndex);
	}
	if (!Source->Component->RemoveInstances(SourceIndices, true))
	{
		TargetIndices.Sort(TGreater<int32>());
		Target->Component->RemoveInstances(TargetIndices, true);
		DestroyClusterIfEmpty(TargetKey);
		return false;
	}

	for (const FRemoval& Removal : SourceRemovals)
	{
		const int32 LastIndex = Source->HandlesByInstanceIndex.Num() - 1;
		check(Source->HandlesByInstanceIndex.IsValidIndex(Removal.InstanceIndex));
		check(Source->HandlesByInstanceIndex[Removal.InstanceIndex] == Removal.Instance);
		if (Removal.InstanceIndex != LastIndex)
		{
			const FMeshPoolInstanceHandle Moved = Source->HandlesByInstanceIndex[LastIndex];
			Source->HandlesByInstanceIndex[Removal.InstanceIndex] = Moved;
			Data->Locations.FindChecked(Moved).InstanceIndex = Removal.InstanceIndex;
		}
		Source->HandlesByInstanceIndex.Pop(EAllowShrinking::No);
	}
	for (int32 Index = 0; Index < Updates.Num(); ++Index)
	{
		Target->HandlesByInstanceIndex.Add(Updates[Index].Instance);
		FPhysicalLocation& Location = Data->Locations.FindChecked(Updates[Index].Instance);
		Location.Cluster = TargetKey;
		Location.InstanceIndex = TargetIndices[Index];
	}
	MarkClusterDirty(SourceKey);
	MarkClusterDirty(TargetKey);
	DestroyClusterIfEmpty(SourceKey);
	return true;
}

bool AMeshPoolRenderHost::RemoveInstances(const TConstArrayView<FMeshPoolInstanceHandle> Instances)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Presentation_MeshPool_Remove);
	CSV_SCOPED_TIMING_STAT(ElementSandboxPresentation, MeshPoolRemove);
	check(IsInGameThread());
	if (!Data)
	{
		return false;
	}
	if (Instances.IsEmpty())
	{
		return true;
	}
	TSet<FMeshPoolInstanceHandle> Unique;
	TMap<FMeshPoolClusterKey, FRemovalBatch> Batches;
	for (const FMeshPoolInstanceHandle Instance : Instances)
	{
		const FPhysicalLocation* Location = Data->Locations.Find(Instance);
		if (!Location || Unique.Contains(Instance))
		{
			return false;
		}
		Unique.Add(Instance);
		Batches.FindOrAdd(Location->Cluster).Items.Add({Instance, Location->InstanceIndex});
	}
	for (TPair<FMeshPoolClusterKey, FRemovalBatch>& Pair : Batches)
	{
		FMeshPoolHostCluster* Cluster = Data->Clusters.Find(Pair.Key);
		if (!Cluster || !Cluster->Component)
		{
			return false;
		}
		Pair.Value.Items.Sort([](const FRemoval& Left, const FRemoval& Right)
							  { return Left.InstanceIndex > Right.InstanceIndex; });
		TArray<int32> Indices;
		for (const FRemoval& Removal : Pair.Value.Items)
		{
			Indices.Add(Removal.InstanceIndex);
		}
		if (!Cluster->Component->RemoveInstances(Indices, true))
		{
			return false;
		}
		for (const FRemoval& Removal : Pair.Value.Items)
		{
			const int32 LastIndex = Cluster->HandlesByInstanceIndex.Num() - 1;
			check(Cluster->HandlesByInstanceIndex.IsValidIndex(Removal.InstanceIndex));
			check(Cluster->HandlesByInstanceIndex[Removal.InstanceIndex] == Removal.Instance);
			if (Removal.InstanceIndex != LastIndex)
			{
				const FMeshPoolInstanceHandle Moved = Cluster->HandlesByInstanceIndex[LastIndex];
				Cluster->HandlesByInstanceIndex[Removal.InstanceIndex] = Moved;
				Data->Locations.FindChecked(Moved).InstanceIndex = Removal.InstanceIndex;
			}
			Cluster->HandlesByInstanceIndex.Pop(EAllowShrinking::No);
			Data->Locations.Remove(Removal.Instance);
		}
		MarkClusterDirty(Pair.Key);
		DestroyClusterIfEmpty(Pair.Key);
	}
	return true;
}

void AMeshPoolRenderHost::ClearLayer(const FMeshPoolLayerHandle Layer)
{
	check(IsInGameThread());
	if (!Data || !Layer.IsSet())
	{
		return;
	}
	TArray<FMeshPoolClusterKey> Keys;
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (Pair.Key.Layer == Layer)
		{
			Keys.Add(Pair.Key);
		}
	}
	for (const FMeshPoolClusterKey& Key : Keys)
	{
		FMeshPoolHostCluster* Cluster = Data->Clusters.Find(Key);
		if (!Cluster)
		{
			continue;
		}
		for (const FMeshPoolInstanceHandle Instance : Cluster->HandlesByInstanceIndex)
		{
			Data->Locations.Remove(Instance);
		}
		if (Cluster->Component)
		{
			Data->DirtyComponents.Remove(Cluster->Component);
			Cluster->Component->ClearInstances();
		}
		Cluster->HandlesByInstanceIndex.Reset();

		// ClearLayer 也可能撞上正在后台建树的 HISM。统一交给空 Cluster 退役路径，
		// 让逻辑实例立即消失，物理 Component 等异步任务自然结束后再释放。
		DestroyClusterIfEmpty(Key);
	}
}

void AMeshPoolRenderHost::ClearAll()
{
	check(IsInGameThread());
	if (!Data)
	{
		return;
	}
	for (TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			AccumulateRetiredTreeBuildStats(*Data, *Hierarchical);
		}
		if (Pair.Value.Component)
		{
			RemoveInstanceComponent(Pair.Value.Component);
			Pair.Value.Component->DestroyComponent();
		}
	}
	Data->Clusters.Reset();
	Data->Locations.Reset();
	Data->DirtyComponents.Reset();
	Data->PendingDestroyClusters.Reset();
	Data->BulkEditDepth = 0;
}

bool AMeshPoolRenderHost::IsValidInstance(const FMeshPoolInstanceHandle Instance) const
{
	return Data && Data->Locations.Contains(Instance);
}

bool AMeshPoolRenderHost::TryGetInstanceTransform(const FMeshPoolInstanceHandle Instance,
												  FTransform& OutTransform) const
{
	const FPhysicalLocation* Location = Data ? Data->Locations.Find(Instance) : nullptr;
	const FMeshPoolHostCluster* Cluster = Location ? Data->Clusters.Find(Location->Cluster) : nullptr;
	return Cluster && Cluster->Component &&
		   Cluster->Component->GetInstanceTransform(Location->InstanceIndex, OutTransform, true);
}

int32 AMeshPoolRenderHost::GetInstanceCount() const
{
	return Data ? Data->Locations.Num() : 0;
}

int32 AMeshPoolRenderHost::GetClusterCount() const
{
	// 正在等待异步树退出的空 Component 是物理退休资源，不再属于逻辑 Cluster 统计。
	return Data ? Data->Clusters.Num() - Data->PendingDestroyClusters.Num() : 0;
}

uint64 AMeshPoolRenderHost::GetHierarchicalTreeBuildRequestCount() const
{
	if (!Data)
	{
		return 0;
	}
	uint64 Count = Data->RetiredTreeBuildRequests;
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			Count += Hierarchical->GetTreeBuildRequestCount();
		}
	}
	return Count;
}

uint64 AMeshPoolRenderHost::GetHierarchicalTreeBuildRetryCount() const
{
	if (!Data) return 0;
	uint64 Count = Data->RetiredTreeBuildRetries;
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
			Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			Count += Hierarchical->GetTreeBuildRetryCount();
		}
	}
	return Count;
}

uint64 AMeshPoolRenderHost::GetHierarchicalTreeBuildDeferredRequestCount() const
{
	if (!Data)
	{
		return 0;
	}
	uint64 Count = Data->RetiredDeferredTreeBuildRequests;
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			Count += Hierarchical->GetDeferredTreeBuildRequestCount();
		}
	}
	return Count;
}

uint64 AMeshPoolRenderHost::GetHierarchicalTreeBuildCoalescedRequestCount() const
{
	if (!Data)
	{
		return 0;
	}
	uint64 Count = Data->RetiredCoalescedTreeBuildRequests;
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		if (const UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
				Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Pair.Value.Component))
		{
			Count += Hierarchical->GetCoalescedTreeBuildRequestCount();
		}
	}
	return Count;
}

SIZE_T AMeshPoolRenderHost::GetEstimatedCPUAllocatedSize() const
{
	if (!Data)
	{
		return 0;
	}
	SIZE_T Size = Data->Clusters.GetAllocatedSize() + Data->Locations.GetAllocatedSize() +
				  Data->DirtyComponents.GetAllocatedSize() + Data->PendingDestroyClusters.GetAllocatedSize();
	for (const TPair<FMeshPoolClusterKey, FMeshPoolHostCluster>& Pair : Data->Clusters)
	{
		Size += Pair.Value.HandlesByInstanceIndex.GetAllocatedSize();
		Size += static_cast<SIZE_T>(Pair.Value.HandlesByInstanceIndex.Num()) * sizeof(FInstancedStaticMeshInstanceData);
	}
	return Size;
}

UInstancedStaticMeshComponent* AMeshPoolRenderHost::GetClusterComponent(const FMeshPoolClusterKey& Cluster) const
{
	const FMeshPoolHostCluster* Found = Data ? Data->Clusters.Find(Cluster) : nullptr;
	return Found ? Found->Component : nullptr;
}

void AMeshPoolRenderHost::MarkClusterDirty(const FMeshPoolClusterKey& Cluster)
{
	FMeshPoolHostCluster* Found = Data ? Data->Clusters.Find(Cluster) : nullptr;
	if (!Found || !Found->Component)
	{
		return;
	}
	if (Data->BulkEditDepth > 0)
	{
		Data->DirtyComponents.Add(Found->Component);
	}
	else
	{
		Found->Component->MarkRenderInstancesDirty();
	}
}

void AMeshPoolRenderHost::DestroyClusterIfEmpty(const FMeshPoolClusterKey& Key)
{
	FMeshPoolHostCluster* Cluster = Data ? Data->Clusters.Find(Key) : nullptr;
	if (!Cluster || !Cluster->HandlesByInstanceIndex.IsEmpty())
	{
		return;
	}
	if (Data->BulkEditDepth > 0)
	{
		Data->PendingDestroyClusters.Add(Key);
		return;
	}
	if (UMeshPoolHierarchicalInstancedStaticMeshComponent* Hierarchical =
			Cast<UMeshPoolHierarchicalInstancedStaticMeshComponent>(Cluster->Component))
	{
		if (Hierarchical->IsAsyncBuilding())
		{
			Data->PendingDestroyClusters.Add(Key);
			return;
		}
		AccumulateRetiredTreeBuildStats(*Data, *Hierarchical);
	}
	if (Cluster->Component)
	{
		RemoveInstanceComponent(Cluster->Component);
		Cluster->Component->DestroyComponent();
	}
	Data->Clusters.Remove(Key);
}
