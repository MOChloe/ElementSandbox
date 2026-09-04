#include "Presentation/WoodProductBatchSubmitter.h"
#include "Presentation/DeferredHISMComponent.h"
#include "WorldObjects/WoodProductFlightMaterialSet.h"
#include "WorldObjects/WoodProductPresentationSettings.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"

namespace
{
	using FProductGroupKey = FWoodProductInstanceStore::FGroupKey;
	FVector ScaleFor(FName Product) { return Product == TEXT("WorldObject.Charcoal") ? FVector(0.45, 0.65, 0.65) : FVector::OneVector; }
	FTransform RenderTransform(const FWoodProductInstanceStore::FEntry& Entry)
	{
		FTransform Result = Entry.Transform;
		Result.SetScale3D(Result.GetScale3D() * ScaleFor(Entry.Product));
		return Result;
	}
	void EnsureFlightMaterial(FWoodProductInstanceStore::FGroup& Group, const FProductGroupKey& Key,
		UWoodProductFlightMaterialSet& Materials, int32 RequiredTier)
	{
		auto* Component = Group.Component.Get();
		if (!Component || RequiredTier == INDEX_NONE) return;
		if (!Group.bFlightMaterial)
		{
			Component->SetNumCustomDataFloats(FWoodProductFlight::CustomFloatCount);
			// 迟到的同 Cell 成员到来时，原有已落地实例仍必须保持可见。
			if (Component->GetInstanceCount())
			{
				TArray<float> Data;
				Data.SetNumZeroed(Component->GetInstanceCount() * FWoodProductFlight::CustomFloatCount);
				for (int32 I = 0; I < Component->GetInstanceCount(); ++I)
					Data[I * FWoodProductFlight::CustomFloatCount + FWoodProductFlight::PhaseIndex] = 2.0f;
				Component->SetCustomDataRange(0, Component->GetInstanceCount() - 1, Data);
			}
			Group.bFlightMaterial = true;
		}
		// WPO 档位是共享组件的临时状态，不是分组边界。组内出现更大轨迹时原地升级；
		// 为避免反复重建 Bounds，在本轮全部飞行结束前不降档。
		if (RequiredTier > Group.FlightMaterialTier)
		{
			Component->SetMaterial(0, Materials.GetMaterial(Key.Product == TEXT("WorldObject.Charcoal"), RequiredTier));
			Group.FlightMaterialTier = RequiredTier;
		}
	}
}
void FWoodProductBatchSubmitter::Apply(FWoodProductInstanceStore& Store, AActor& Host,
	UWoodProductFlightMaterialSet& Materials, const UWoodProductPresentationSettings& Settings,
	TConstArrayView<FWorldEntityId> Work, double Now)
{
	TMap<FProductGroupKey, TArray<FWorldEntityId>> Removes, Writes;
	// DeferredHISM 自发请求也使用平台时钟；飞行时间只用于材质，不可用于建树截止时刻。
	const double EditTime = FPlatformTime::Seconds();
	TArray<FWorldEntityId> Erase;
	for (FWorldEntityId Id : Work)
	{
		auto* Entry = Store.Entries.Find(Id);
		if (!Entry) continue;
		Entry->bQueued = false;
		if (Entry->bRemoved)
		{
			if (Entry->Index != INDEX_NONE) Removes.FindOrAdd(Entry->Group).Add(Id);
			Erase.Add(Id); continue;
		}
		if (Entry->Product.IsNone()) continue; // 仅 Settlement 到达，没有位置。
		const FVector P = Entry->Transform.GetLocation();
		FProductGroupKey Desired;
		Desired.Product = Entry->Product;
		Desired.Cell = FIntPoint(FMath::FloorToInt(P.X / Settings.CellSize), FMath::FloorToInt(P.Y / Settings.CellSize));
		if (const auto* Flight = Store.FindFlight(*Entry); Flight && !Entry->bPersistent
			&& Materials.FindTier(Flight->GetDisplacementExtent()) == INDEX_NONE)
		{
			UE_LOG(LogTemp, Error, TEXT("Wood flight displacement %.0f cm exceeds generated material tiers."), Flight->GetDisplacementExtent());
			Store.Queue(Id); continue;
		}
		if (Entry->Index != INDEX_NONE && Entry->Group.Product == Desired.Product && Entry->Group.Cell == Desired.Cell)
		{
			// 落地认领不改变组件或槽位。后续普通物件只有实际跨 Cell 才迁移。
			Desired = Entry->Group;
		}
		else
		{
			if (Entry->Index != INDEX_NONE) Removes.FindOrAdd(Entry->Group).Add(Id);
		}
		Writes.FindOrAdd(Desired).Add(Id);
	}
	for (auto& Pair : Removes)
	{
		auto* GroupPtr = Store.Groups.Find(Pair.Key);
		if (!GroupPtr) continue;
		auto& Group = **GroupPtr;
		auto* Component = Group.Component.Get();
		Pair.Value.Sort([&](FWorldEntityId A, FWorldEntityId B) { return Store.Entries.FindChecked(A).Index > Store.Entries.FindChecked(B).Index; });
		TArray<int32> Indices;
		for (FWorldEntityId Id : Pair.Value) Indices.Add(Store.Entries.FindChecked(Id).Index);
		Component->BeginBulkEdit();
		verify(Component->RemoveInstances(Indices, true));
		Component->EndBulkEdit(EditTime, true);
		for (FWorldEntityId Id : Pair.Value)
		{
			auto& Entry = Store.Entries.FindChecked(Id);
			const int32 Index = Entry.Index;
			Group.PreparedCount -= Entry.bPreparedOnGPU ? 1 : 0; Entry.bPreparedOnGPU = false;
			Group.Owners.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			if (Group.Owners.IsValidIndex(Index)) Store.Entries.FindChecked(Group.Owners[Index]).Index = Index;
			Entry.Index = INDEX_NONE; ++Store.TotalRemoves;
		}
		Component->SetVisibility(Group.Owners.Num() > Group.PreparedCount);
		++Store.NativeBatches;
		Store.ScheduleGroup(Pair.Key);
	}
	for (auto& Pair : Writes)
	{
		auto& GroupPtr = Store.Groups.FindOrAdd(Pair.Key);
		if (!GroupPtr)
		{
			GroupPtr = MakeUnique<FWoodProductInstanceStore::FGroup>();
			auto* Component = NewObject<UDeferredHISMComponent>(&Host, NAME_None, RF_Transient);
			Component->SetupAttachment(Host.GetRootComponent());
			Component->SetRelativeLocation(FVector(Pair.Key.Cell.X * Settings.CellSize, Pair.Key.Cell.Y * Settings.CellSize, 0));
			Component->SetStaticMesh(Materials.Mesh);
			Component->SetMaterial(0, Materials.GetMaterial(Pair.Key.Product == TEXT("WorldObject.Charcoal"), INDEX_NONE));
			Component->SetRemoveSwap(); Component->SetMobility(EComponentMobility::Movable);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetGenerateOverlapEvents(false); Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(true);
			// 在首批参数提交前保持隐藏；全 Prepared 组件不应进入绘制提交。
			Component->SetVisibility(false);
			Component->SetNumCustomDataFloats(0);
			Host.AddInstanceComponent(Component); Component->RegisterComponent(); Component->PrewarmEmptyTree();
			GroupPtr->Component = Component;
		}
		auto& Group = *GroupPtr;
		auto* Component = Group.Component.Get();
		TArray<FTransform> Adds;
		TArray<FWorldEntityId> AddedIds;
		bool bTransformEdited = false;
		Component->BeginBulkEdit();
		for (FWorldEntityId Id : Pair.Value)
		{
			auto& Entry = Store.Entries.FindChecked(Id);
			const FTransform Transform = RenderTransform(Entry);
			if (Entry.Index == INDEX_NONE) { Adds.Add(Transform); AddedIds.Add(Id); }
			else
			{
				FTransform Current;
				Component->GetInstanceTransform(Entry.Index, Current, true);
				if (!Current.Equals(Transform, 0.01))
				{
					verify(Component->UpdateInstanceTransform(Entry.Index, Transform, true, false, true));
					++Store.TotalTransformUpdates; bTransformEdited = true;
				}
			}
		}
		if (!Adds.IsEmpty())
		{
			Component->PreAllocateInstancesMemory(Adds.Num());
			const TArray<int32> Indices = Component->AddInstances(Adds, true, true, false);
			check(Indices.Num() == AddedIds.Num());
			for (int32 I = 0; I < Indices.Num(); ++I)
			{
				auto& Entry = Store.Entries.FindChecked(AddedIds[I]);
				check(Indices[I] == Group.Owners.Num());
				Entry.Index = Indices[I]; Entry.Group = Pair.Key; Group.Owners.Add(AddedIds[I]);
			}
			Store.TotalAdds += Adds.Num(); bTransformEdited = true;
		}
		int32 RequiredFlightTier = INDEX_NONE;
		for (FWorldEntityId Id : Pair.Value)
			if (const auto* Flight = Store.FindFlight(Store.Entries.FindChecked(Id)))
				RequiredFlightTier = FMath::Max(RequiredFlightTier, Materials.FindTier(Flight->GetDisplacementExtent()));
		if (RequiredFlightTier != INDEX_NONE) EnsureFlightMaterial(Group, Pair.Key, Materials, RequiredFlightTier);
		if (Group.bFlightMaterial)
		{
			Pair.Value.Sort([&](FWorldEntityId A, FWorldEntityId B) { return Store.Entries.FindChecked(A).Index < Store.Entries.FindChecked(B).Index; });
			TArray<float> Data;
			int32 First = INDEX_NONE, Last = INDEX_NONE;
			auto Flush = [&]()
			{
				if (First != INDEX_NONE) verify(Component->SetCustomDataRange(First, Last, Data));
				Data.Reset(); First = Last = INDEX_NONE;
			};
			for (FWorldEntityId Id : Pair.Value)
			{
				auto& Entry = Store.Entries.FindChecked(Id);
				const auto* Flight = Store.FindFlight(Entry);
				if (Last != INDEX_NONE && Entry.Index != Last + 1) Flush();
				if (First == INDEX_NONE) First = Entry.Index;
				Last = Entry.Index;
				Group.PreparedCount -= Entry.bPreparedOnGPU ? 1 : 0;
				Entry.bPreparedOnGPU = Flight && Flight->Phase == EWoodProductFlightPhase::Prepared;
				Group.PreparedCount += Entry.bPreparedOnGPU ? 1 : 0;
				if (Flight)
				{
					Flight->PackCustomData(Data);
					if (Flight->Phase == EWoodProductFlightPhase::Active)
						Group.LastEndTime = FMath::Max(Group.LastEndTime, Flight->LocalStartTime + Flight->ImpactSeconds + Flight->SettlingSeconds);
				}
				else
				{
					const int32 Start = Data.AddZeroed(FWoodProductFlight::CustomFloatCount);
					Data[Start + FWoodProductFlight::PhaseIndex] = 2.0f;
				}
			}
			Flush();
		}
		Component->EndBulkEdit(EditTime, bTransformEdited);
		// 混合组仍由每实例 Phase 隐藏 Prepared；激活、终态和普通认领立即恢复整组可见性。
		Component->SetVisibility(Group.Owners.Num() > Group.PreparedCount);
		++Store.NativeBatches;
		Store.ScheduleGroup(Pair.Key);
	}
	for (FWorldEntityId Id : Erase) { Store.ReleaseFlight(Store.Entries.FindChecked(Id)); Store.Entries.Remove(Id); }
}
void FWoodProductBatchSubmitter::Tick(FWoodProductInstanceStore& Store, AActor& Host,
	UWoodProductFlightMaterialSet& Materials, const UWoodProductPresentationSettings& Settings, double Now)
{
	const double Start = FPlatformTime::Seconds();
	const double Deadline = Start + Settings.InstanceApplyTargetMilliseconds / 1000.0;
	// 激活/认领/删除先提交；全 Prepared 组的循环维护不能把状态更新饿死在隐藏态。
	// 至少推进一个有界原生批次，并给建树/清理留出尾段，双方都不能依赖对方队列清空。
	const double ApplyDeadline = Start + Settings.InstanceApplyTargetMilliseconds * 0.00075;
	bool bAppliedBatch = false;
	while (Store.PendingHead < Store.Pending.Num() && (!bAppliedBatch || FPlatformTime::Seconds() < ApplyDeadline))
	{
		const int32 Count = FMath::Min(Settings.MaximumNativeInstanceBatchSize, Store.Pending.Num() - Store.PendingHead);
		TArray<FWorldEntityId> Work;
		Work.Append(Store.Pending.GetData() + Store.PendingHead, Count);
		Store.PendingHead += Count;
		Apply(Store, Host, Materials, Settings, Work, Now);
		bAppliedBatch = true;
	}
	if (Store.PendingHead == Store.Pending.Num()) { Store.Pending.Reset(); Store.PendingHead = 0; }
	else if (Store.PendingHead >= 4096) { Store.Pending.RemoveAt(0, Store.PendingHead, EAllowShrinking::No); Store.PendingHead = 0; }
	const int32 MaintenanceEnd = Store.Maintenance.Num();
	bool bMaintainedGroup = false;
	while (Store.MaintenanceHead < MaintenanceEnd && (!bMaintainedGroup || FPlatformTime::Seconds() < Deadline))
	{
		bMaintainedGroup = true;
		const auto Key = Store.Maintenance[Store.MaintenanceHead++];
		auto& Group = *Store.Groups.FindChecked(Key);
		Group.bScheduled = false;
		auto* Component = Group.Component.Get();
		if (Group.Owners.IsEmpty())
		{
			Component->DestroyComponent(); Store.Groups.Remove(Key); continue;
		}
		if (!Component->IsAsyncBuilding()) Component->NotifyAsyncBuildObservedComplete();
		if (Component->HasPendingTreeBuild())
			Component->TryStartDeferredTreeBuild(Start, Settings.TreeBuildQuietSeconds, Settings.TreeBuildMaxDeferralSeconds);
		if (Group.bFlightMaterial && Group.PreparedCount == 0 && Now >= Group.LastEndTime && Store.Pending.IsEmpty())
		{
			// 原组件收回 WPO 范围并释放临时参数；身份、槽位和最终 Transform 均不变。
			Component->SetMaterial(0, Materials.GetMaterial(Key.Product == TEXT("WorldObject.Charcoal"), INDEX_NONE));
			Component->SetNumCustomDataFloats(0);
			Group.bFlightMaterial = false;
			Group.FlightMaterialTier = INDEX_NONE;
			Group.LastEndTime = 0.0;
			for (FWorldEntityId Id : Group.Owners)
			{
				auto& Entry = Store.Entries.FindChecked(Id);
				Store.ReleaseFlight(Entry); Entry.bVisualFinished = true;
				if (!Entry.bPersistent && !Store.IsRetained(Entry)) Store.Remove(Id);
			}
		}
		if (Group.bFlightMaterial || Component->HasPendingTreeBuild() || Component->IsAsyncBuilding()) Store.ScheduleGroup(Key);
	}
	if (Store.MaintenanceHead == Store.Maintenance.Num()) { Store.Maintenance.Reset(); Store.MaintenanceHead = 0; }
	else if (Store.MaintenanceHead >= 4096) { Store.Maintenance.RemoveAt(0, Store.MaintenanceHead, EAllowShrinking::No); Store.MaintenanceHead = 0; }
	Store.LastApplyMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
}
