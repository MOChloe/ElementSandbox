#include "MeteorClientRuntime.h"

namespace UE::ElementSandbox::Meteor
{
FMeteorClientDirectoryEntry* FMeteorClientPagedDirectory::Find(uint32 Ordinal)
{
	const uint32 Page = Ordinal / ClientDirectoryPageCapacity;
	return Pages.IsValidIndex(Page) && Pages[Page] ? &Pages[Page]->Entries[Ordinal % ClientDirectoryPageCapacity] : nullptr;
}
const FMeteorClientDirectoryEntry* FMeteorClientPagedDirectory::Find(uint32 Ordinal) const
{
	return const_cast<FMeteorClientPagedDirectory*>(this)->Find(Ordinal);
}
FMeteorClientDirectoryEntry* FMeteorClientPagedDirectory::FindOrAdd(uint32 Ordinal)
{
	const uint32 Page = Ordinal / ClientDirectoryPageCapacity;
	if (Page > 65535) return nullptr;
	while (Pages.Num() <= static_cast<int32>(Page)) Pages.Add(nullptr);
	if (!Pages[Page]) Pages[Page] = MakeUnique<FPage>();
	return &Pages[Page]->Entries[Ordinal % ClientDirectoryPageCapacity];
}
void FMeteorClientPagedDirectory::Reset() { Pages.Reset(); }
SIZE_T FMeteorClientPagedDirectory::GetAllocatedBytes() const
{
	SIZE_T Bytes = Pages.GetAllocatedSize();
	for (const auto& Page : Pages) if (Page) Bytes += sizeof(FPage);
	return Bytes;
}
bool FMeteorClientRuntime::Initialize(const FMeteorRuntimeConfig& Config)
{
	Reset();
	bInitialized = Config.IsValid();
	return bInitialized;
}
void FMeteorClientRuntime::Reset()
{
	Directory.Reset(); PreparedPages.Reset(); PageRevisions.Reset(); CanceledPages.Reset();
	PendingOrdinals.Reset(); Stats = {}; bInitialized = false;
}
void FMeteorClientRuntime::SetState(FMeteorClientDirectoryEntry& Entry, EMeteorClientDebrisState State)
{
	if (Entry.State == State) return;
	if (Entry.State == EMeteorClientDebrisState::Prepared) --Stats.PreparedLaneCount;
	if (Entry.State == EMeteorClientDebrisState::Flying) --Stats.FlyingLaneCount;
	if (Entry.State == EMeteorClientDebrisState::Settled) --Stats.SettledLaneCount;
	Entry.State = State;
	if (State == EMeteorClientDebrisState::Prepared) ++Stats.PreparedLaneCount;
	if (State == EMeteorClientDebrisState::Flying) ++Stats.FlyingLaneCount;
	if (State == EMeteorClientDebrisState::Settled) ++Stats.SettledLaneCount;
}
void FMeteorClientRuntime::Queue(uint32 Ordinal)
{
	auto* Entry = Directory.Find(Ordinal);
	if (Entry && !Entry->bPresentationQueued)
	{
		Entry->bPresentationQueued = true;
		PendingOrdinals.Add(Ordinal);
		Stats.PendingPresentationCount = PendingOrdinals.Num();
	}
}
bool FMeteorClientRuntime::PrepareTrajectoryPage(TSharedPtr<const FMeteorTrajectoryPage> Page)
{
	if (!bInitialized || !Page || !Page->IsValid()) return false;
	if (const uint32* Canceled = CanceledPages.Find(Page->PageId); Canceled && *Canceled >= Page->Revision) return false;
	const uint32 ExistingRevision = GetPreparedPageRevision(Page->PageId);
	if (ExistingRevision >= Page->Revision) return ExistingRevision == Page->Revision;
	// 页必须整体校验后才修改目录，避免坏身份留下部分可见实例。
	for (int32 Lane = 0; Lane < Page->Num(); ++Lane)
	{
		auto* Entry = Directory.FindOrAdd(Page->Ordinals[Lane]);
		if (!Entry || (Entry->WorldEntityId.IsSet() && Entry->WorldEntityId != Page->WorldEntityIds[Lane])) return false;
	}
	PageRevisions.Add(Page->PageId, Page->Revision);
	PreparedPages.Add(Page->PageId, Page);
	for (int32 Lane = 0; Lane < Page->Num(); ++Lane)
	{
		auto& Entry = *Directory.Find(Page->Ordinals[Lane]);
		Entry.WorldEntityId = Page->WorldEntityIds[Lane]; Entry.PageId = Page->PageId;
		Entry.PageLane = Lane; Entry.SegmentRevision = Page->Revision;
		if (Entry.State == EMeteorClientDebrisState::Missing) SetState(Entry, EMeteorClientDebrisState::Prepared);
		Queue(Page->Ordinals[Lane]);
	}
	Stats.PreparedPageCount = PreparedPages.Num();
	return true;
}
int32 FMeteorClientRuntime::ActivateTrajectoryLanes(uint64 PageId, uint32 Revision,
	TConstArrayView<uint32> Ordinals, double StartTime)
{
	if (!bInitialized || !HasPreparedPage(PageId, Revision) || !FMath::IsFinite(StartTime) || StartTime < 0) return 0;
	int32 Count = 0;
	for (uint32 Ordinal : Ordinals)
	{
		auto* Entry = Directory.Find(Ordinal);
		if (!Entry || Entry->PageId != PageId || Entry->SegmentRevision != Revision
			|| Entry->State != EMeteorClientDebrisState::Prepared) continue;
		SetState(*Entry, EMeteorClientDebrisState::Flying);
		Entry->AuthorityStartTimeSeconds = StartTime;
		Queue(Ordinal); ++Count;
	}
	if (!Stats.TotalActivatedLaneCount && Count) Stats.FirstActivationLaneCount = Count;
	Stats.TotalActivatedLaneCount += Count;
	return Count;
}
void FMeteorClientRuntime::CancelTrajectoryLanes(uint64 PageId, uint32 Revision, TConstArrayView<uint32> Ordinals)
{
	for (uint32 Ordinal : Ordinals)
	{
		auto* Entry = Directory.FindOrAdd(Ordinal);
		if (!Entry || (Entry->PageId && Entry->PageId != PageId) || Entry->SegmentRevision > Revision
			|| Entry->State == EMeteorClientDebrisState::Settled) continue;
		Entry->PageId = PageId; Entry->SegmentRevision = Revision;
		SetState(*Entry, EMeteorClientDebrisState::Canceled); Queue(Ordinal);
	}
}
void FMeteorClientRuntime::CancelTrajectoryPage(uint64 PageId, uint32 Revision)
{
	CanceledPages.FindOrAdd(PageId) = FMath::Max(CanceledPages.FindRef(PageId), Revision);
	if (auto* Page = PreparedPages.Find(PageId); Page && (*Page)->Revision <= Revision)
		CancelTrajectoryLanes(PageId, Revision, (*Page)->Ordinals);
}
uint32 FMeteorClientRuntime::GetPreparedPageRevision(uint64 PageId) const { return FMath::Max(PageRevisions.FindRef(PageId), CanceledPages.FindRef(PageId)); }
bool FMeteorClientRuntime::HasPreparedPage(uint64 PageId, uint32 Revision) const { return GetPreparedPageRevision(PageId) == Revision; }
bool FMeteorClientRuntime::MarkSettled(uint32 Ordinal, FWorldEntityId Id)
{
	auto* Entry = Directory.FindOrAdd(Ordinal);
	if (!Entry || !Id.IsSet() || (Entry->WorldEntityId.IsSet() && Entry->WorldEntityId != Id)) return false;
	if (Entry->State == EMeteorClientDebrisState::Canceled) return false;
	if (Entry->State == EMeteorClientDebrisState::Settled) return true;
	Entry->WorldEntityId = Id; SetState(*Entry, EMeteorClientDebrisState::Settled); Queue(Ordinal);
	return true;
}
void FMeteorClientRuntime::ConsumePresentationChanges(TArray<FMeteorClientPresentationLane>& Out, double Offset)
{
	Out.Reset(PendingOrdinals.Num());
	TSet<uint64> TouchedPages;
	for (uint32 Ordinal : PendingOrdinals)
	{
		auto& Entry = *Directory.Find(Ordinal);
		Entry.bPresentationQueued = false;
		if (!Entry.WorldEntityId.IsSet()) continue;
		auto* PagePtr = PreparedPages.Find(Entry.PageId);
		const auto* Page = PagePtr ? PagePtr->Get() : nullptr;
		auto& Item = Out.AddDefaulted_GetRef();
		Item.Ordinal = Ordinal; Item.WorldEntityId = Entry.WorldEntityId; Item.State = Entry.State;
		Item.PageId = Entry.PageId; Item.Revision = Entry.SegmentRevision;
		if (!Page || !Page->Ordinals.IsValidIndex(Entry.PageLane)) continue;
		const int32 Lane = Entry.PageLane;
		Item.BurstId = Page->BurstId; Item.RenderArchetypeId = Page->RenderArchetypeId;
		Item.StartPosition = FVector(Page->PageOrigin) + FVector(Page->LocalStarts[Lane]);
		Item.ImpactEndpoint = FVector(Page->PageOrigin) + FVector(Page->LocalImpactEndpoints[Lane]);
		Item.RestTransform = Page->GetRestTransform(Lane);
		Item.InitialVelocity = Page->InitialVelocities[Lane]; Item.Acceleration = Page->Accelerations[Lane];
		Item.AngularVelocityDegrees = Page->AngularVelocitiesDegrees[Lane]; Item.StartRotation = Page->StartRotations[Lane];
		Item.VisualRadius = Page->VisualRadii[Lane]; Item.LocalStartTimeSeconds = Entry.AuthorityStartTimeSeconds + Offset;
		Item.ImpactDurationSeconds = Page->ImpactDurations[Lane]; Item.SettlingDurationSeconds = Page->SettlingDurations[Lane];
		Item.SettlingLiftHeight = Page->SettlingLiftHeights[Lane]; TouchedPages.Add(Entry.PageId);
	}
	PendingOrdinals.Reset(); Stats.PendingPresentationCount = 0;
	for (uint64 Id : TouchedPages)
	{
		const auto Page = PreparedPages.FindRef(Id);
		bool bTerminal = true;
		for (uint32 Ordinal : Page->Ordinals)
		{
			const auto State = Directory.Find(Ordinal)->State;
			if (State != EMeteorClientDebrisState::Settled && State != EMeteorClientDebrisState::Canceled) { bTerminal = false; break; }
		}
		if (bTerminal) PreparedPages.Remove(Id);
	}
	Stats.PreparedPageCount = PreparedPages.Num();
}
void FMeteorClientRuntime::SetPipelineStats(int32 Prepared, int32 Activation, double PrepareMs, double ActivateMs, bool Exhausted)
{
	Stats.PreparedBacklog = Prepared; Stats.ActivationBacklog = Activation;
	Stats.PrepareMilliseconds = PrepareMs; Stats.ActivationMilliseconds = ActivateMs;
	Stats.bGameThreadBudgetExhausted = Exhausted;
}
}
