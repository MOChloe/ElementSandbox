#include "Presentation/WoodProductInstanceStore.h"

FWoodProductFlight* FWoodProductInstanceStore::FindFlight(const FEntry& Entry)
{
	return Entry.FlightIndex == INDEX_NONE ? nullptr : &FlightData[Entry.FlightIndex];
}
const FWoodProductFlight* FWoodProductInstanceStore::FindFlight(const FEntry& Entry) const
{
	return Entry.FlightIndex == INDEX_NONE ? nullptr : &FlightData[Entry.FlightIndex];
}
void FWoodProductInstanceStore::SetFlight(FEntry& Entry, const FWoodProductFlight& Flight)
{
	if (Entry.FlightIndex == INDEX_NONE)
	{
		Entry.FlightIndex = FreeFlightSlots.IsEmpty() ? FlightData.AddDefaulted() : FreeFlightSlots.Pop(EAllowShrinking::No);
		++ActiveFlightCount;
	}
	FlightData[Entry.FlightIndex] = Flight;
}
void FWoodProductInstanceStore::ReleaseFlight(FEntry& Entry)
{
	if (Entry.FlightIndex == INDEX_NONE) return;
	FreeFlightSlots.Add(Entry.FlightIndex); Entry.FlightIndex = INDEX_NONE;
	if (--ActiveFlightCount == 0) { FlightData.Empty(); FreeFlightSlots.Empty(); }
}
void FWoodProductInstanceStore::Queue(FWorldEntityId Id)
{
	auto& Entry = Entries.FindChecked(Id);
	if (!Entry.bQueued) { Entry.bQueued = true; Pending.Add(Id); }
}
void FWoodProductInstanceStore::ScheduleGroup(const FGroupKey& Key)
{
	auto& Group = *Groups.FindChecked(Key);
	if (!Group.bScheduled) { Group.bScheduled = true; Maintenance.Add(Key); }
}
bool FWoodProductInstanceStore::IsRetained(const FEntry& Entry) const
{
	if (!bHasInterest || Entry.Product.IsNone()) return true;
	const FVector Rest = Entry.Transform.GetLocation();
	const auto* Flight = FindFlight(Entry);
	if (!Flight || Entry.bVisualFinished || Flight->Phase == EWoodProductFlightPhase::Settled)
	{
		const auto Chunk = FWorldChunkCoord::FromWorldLocation(Rest);
		for (const auto& Box : RetentionBoxes) if (Box.Contains(Chunk)) return true;
	}
	else
	{
		const FVector Extent(Flight->GetDisplacementExtent());
		const FBox Swept(Rest - Extent, Rest + Extent);
		for (const auto& Box : RetentionBoxes)
			if (Swept.Intersect(FBox(Box.Minimum.GetWorldMinimum(), Box.MaximumExclusive.GetWorldMinimum()))) return true;
	}
	return false;
}
void FWoodProductInstanceStore::SetRetentionBoxes(TArray<FWorldChunkBox>&& Boxes)
{
	bHasInterest |= !Boxes.IsEmpty();
	RetentionBoxes = MoveTemp(Boxes);
	for (const auto& Pair : Entries)
		if (!Pair.Value.bPersistent && !IsRetained(Pair.Value)) Remove(Pair.Key);
}
bool FWoodProductInstanceStore::AcceptFlight(const FWoodProductFlight& Flight)
{
	if (!Flight.WorldEntityId.IsSet() || Flight.BurstId <= RetiredBurst) return false;
	if (Flight.Phase == EWoodProductFlightPhase::Canceled)
	{
		TerminalFlights.Add(Flight.WorldEntityId);
		if (const auto* Entry = Entries.Find(Flight.WorldEntityId); Entry && !Entry->bPersistent) Remove(Flight.WorldEntityId);
		return true;
	}
	if (TerminalFlights.Contains(Flight.WorldEntityId)) return false;
	auto* Existing = Entries.Find(Flight.WorldEntityId);
	if (Existing && (Existing->bPersistent || Existing->bRemoved)) return false;
	if (Existing && Existing->bVisualFinished && Flight.Phase != EWoodProductFlightPhase::Settled) return false;
	if (Flight.Phase == EWoodProductFlightPhase::Settled && !Flight.IsValid())
	{
		// Settlement 可以比 Payload 先到，只记录终态，不凭空创建实例。
		auto& Entry = Entries.FindOrAdd(Flight.WorldEntityId);
		Entry.bServerSettled = true;
		if (auto* CurrentFlight = FindFlight(Entry)) { CurrentFlight->Phase = EWoodProductFlightPhase::Settled; Queue(Flight.WorldEntityId); }
		return true;
	}
	if (!Flight.IsValid()) return false;
	auto& Entry = Entries.FindOrAdd(Flight.WorldEntityId);
	const auto* CurrentFlight = FindFlight(Entry);
	if (CurrentFlight && (CurrentFlight->Revision > Flight.Revision
		|| (CurrentFlight->Phase == EWoodProductFlightPhase::Active && Flight.Phase == EWoodProductFlightPhase::Prepared))) return false;
	Entry.Product = Flight.DefinitionId; Entry.Transform = Flight.RestTransform; SetFlight(Entry, Flight);
	Entry.bServerSettled |= Flight.Phase == EWoodProductFlightPhase::Settled;
	if (Entry.bServerSettled) FindFlight(Entry)->Phase = EWoodProductFlightPhase::Settled;
	if (!IsRetained(Entry)) { Remove(Flight.WorldEntityId); return false; }
	Queue(Flight.WorldEntityId);
	return true;
}
void FWoodProductInstanceStore::Upsert(FWorldEntityId Id, FName Product, const FTransform& Transform)
{
	if (!Id.IsSet()) return;
	auto& Entry = Entries.FindOrAdd(Id);
	if (Entry.bPersistent && !Entry.bRemoved && Entry.Product == Product && Entry.Transform.Equals(Transform)) return;
	if (!Entry.bPersistent && Entry.Index != INDEX_NONE) ++TotalAdoptions;
	Entry.Product = Product; Entry.Transform = Transform;
	Entry.bPersistent = true; Entry.bRemoved = false; Entry.bServerSettled = true;
	if (auto* Flight = FindFlight(Entry)) Flight->Phase = EWoodProductFlightPhase::Settled;
	Queue(Id);
}
void FWoodProductInstanceStore::Remove(FWorldEntityId Id)
{
	TerminalFlights.Add(Id);
	if (auto* Entry = Entries.Find(Id)) { Entry->bRemoved = true; Queue(Id); }
}
void FWoodProductInstanceStore::RetireBurst(uint64 Burst)
{
	RetiredBurst = FMath::Max(RetiredBurst, Burst);
	// 已激活的实例保留到普通对象认领或离开兴趣区；仅未激活准备引用可在此取消。
	for (auto& Pair : Entries)
		if (const auto* Flight = FindFlight(Pair.Value); Flight && Flight->BurstId <= Burst && !Pair.Value.bPersistent
			&& Flight->Phase == EWoodProductFlightPhase::Prepared) Remove(Pair.Key);
	TerminalFlights.Reset(); // 旧 Burst 已由序号屏蔽，不累积历史逐实例墓碑。
}
