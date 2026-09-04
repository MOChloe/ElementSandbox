#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "WorldObjects/WoodProductFlightMaterialSet.h"
#include "WorldObjects/WoodProductPresentationSettings.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"
#include "WorldObjects/CharcoalWorldObjectDefinition.h"
#include "Presentation/WoodProductInstanceStore.h"
#include "Presentation/WoodProductBatchSubmitter.h"
#include "Presentation/DeferredHISMComponent.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"
#include "Chunk/WorldChunkTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DEFINE_CATEGORY(WoodProducts, true);
class FWoodProductPresentationRuntime final
{
public:
	FWoodProductInstanceStore Store;
	FWoodProductBatchSubmitter Submitter;
	TSharedPtr<FStreamableHandle> MaterialLoad;
};
namespace
{
	bool IsWoodProduct(FName Id) { return Id == TEXT("WorldObject.WoodBlock") || Id == TEXT("WorldObject.Charcoal"); }
}
UWoodProductPresentationWorldSubsystem::UWoodProductPresentationWorldSubsystem() = default;
UWoodProductPresentationWorldSubsystem::~UWoodProductPresentationWorldSubsystem() = default;
void UWoodProductPresentationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Runtime = MakePimpl<FWoodProductPresentationRuntime>();
	auto* Objects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
	Objects->RegisterDefinition(*GetMutableDefault<UWoodBlockWorldObjectDefinition>());
	Objects->RegisterDefinition(*GetMutableDefault<UCharcoalWorldObjectDefinition>());
	if (!GetWorld()->IsNetMode(NM_DedicatedServer))
	{
		SnapshotBatchHandle = Objects->OnQuerySnapshotBatchCommitted().AddUObject(this, &ThisClass::HandleSnapshotBatch);
		PopulateInitialSnapshot();
		auto* Storage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>();
		ResidencyHandle = Storage->OnResidencySourcesChanged().AddUObject(this, &ThisClass::RefreshRetention);
		RefreshRetention();
	}
}
void UWoodProductPresentationWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!Runtime || InWorld.IsNetMode(NM_DedicatedServer)) return;
	// 进入世界即预取，不能把首次磁盘加载放在陨石 Prepare 或普通产物的预算 Tick 中。
	Runtime->MaterialLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		FSoftObjectPath(TEXT("/Game/WorldObjects/WoodBlock/DA_WoodProductFlightMaterials.DA_WoodProductFlightMaterials")),
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandleMaterialsLoaded));
}
void UWoodProductPresentationWorldSubsystem::HandleMaterialsLoaded()
{
	check(IsInGameThread());
	if (!Runtime || !Runtime->MaterialLoad || GetWorld()->IsNetMode(NM_DedicatedServer)) return;
	Materials = Runtime->MaterialLoad->GetLoadedAsset<UWoodProductFlightMaterialSet>();
	Runtime->MaterialLoad.Reset(); // UPROPERTY 接管强引用，World 退出时统一释放。
	if (!Materials || !Materials->Mesh || !Materials->StaticWood || !Materials->StaticCharcoal)
	{
		UE_LOG(LogTemp, Error, TEXT("Wood product presentation material set failed to load or is incomplete."));
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RenderHost = GetWorld()->SpawnActor<AActor>(Params);
	if (!RenderHost) return;
	RenderHost->SetReplicates(false); RenderHost->SetActorEnableCollision(false);
	auto* Root = NewObject<USceneComponent>(RenderHost, NAME_None, RF_Transient);
	Root->SetMobility(EComponentMobility::Movable);
	RenderHost->AddInstanceComponent(Root); RenderHost->SetRootComponent(Root); Root->RegisterComponent();
}
void UWoodProductPresentationWorldSubsystem::Deinitialize()
{
	// 取消已排队的完成回调，防止 Travel/退出后再创建 Actor。
	if (Runtime && Runtime->MaterialLoad) Runtime->MaterialLoad->CancelHandle();
	if (auto* Objects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>(); Objects && Objects->HasRuntimeState())
		Objects->OnQuerySnapshotBatchCommitted().Remove(SnapshotBatchHandle);
	if (RenderHost) RenderHost->Destroy();
	if (auto* Storage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>()) Storage->OnResidencySourcesChanged().Remove(ResidencyHandle);
	RenderHost = nullptr; Materials = nullptr; Runtime.Reset();
	Super::Deinitialize();
}
void UWoodProductPresentationWorldSubsystem::PopulateInitialSnapshot()
{
	auto* Objects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
	for (int32 Offset = 0;;)
	{
		FWorldObjectQuerySnapshotPage Page;
		if (!Objects->CopyQuerySnapshotPage(Offset, 1024, Page)) break;
		for (const auto& Shape : Page.Shapes)
			if (IsWoodProduct(Shape.DefinitionId)) Runtime->Store.Upsert(Shape.ShapeRef.WorldEntityId, Shape.DefinitionId, Shape.WorldTransform);
		if (!Page.bHasMore) break;
		Offset = Page.NextOffset;
	}
}
void UWoodProductPresentationWorldSubsystem::HandleSnapshotBatch(const FWorldObjectQuerySnapshotBatch& Batch)
{
	if (!Runtime) return;
	for (const auto& Change : Batch.Changes)
	{
		if (Change.Current && IsWoodProduct(Change.Current->DefinitionId))
			Runtime->Store.Upsert(Change.WorldEntityId, Change.Current->DefinitionId, Change.Current->WorldTransform);
		else if ((Change.Previous && IsWoodProduct(Change.Previous->DefinitionId)) || Runtime->Store.Entries.Contains(Change.WorldEntityId))
			Runtime->Store.Remove(Change.WorldEntityId);
	}
}
void UWoodProductPresentationWorldSubsystem::QueueFlightChanges(TConstArrayView<FWoodProductFlight> Changes)
{
	check(IsInGameThread());
	if (!Runtime || GetWorld()->IsNetMode(NM_DedicatedServer)) return;
	for (const auto& Change : Changes) Runtime->Store.AcceptFlight(Change);
}
void UWoodProductPresentationWorldSubsystem::RetireFlightBurst(uint64 BurstId) { if (Runtime) Runtime->Store.RetireBurst(BurstId); }
void UWoodProductPresentationWorldSubsystem::RefreshRetention()
{
	if (!Runtime) return;
	TArray<FWorldChunkBox> Boxes;
	GetWorld()->GetSubsystem<UWorldStorageSubsystem>()->CopyResidencyRetentionBoxes(Boxes);
	Runtime->Store.SetRetentionBoxes(MoveTemp(Boxes));
}
void UWoodProductPresentationWorldSubsystem::Tick(float DeltaTime)
{
	CSV_SCOPED_TIMING_STAT(WoodProducts, Tick);
	if (!Runtime || !RenderHost || !Materials) return;
	Runtime->Submitter.Tick(Runtime->Store, *RenderHost, *Materials, *GetDefault<UWoodProductPresentationSettings>(), GetWorld()->GetTimeSeconds());
	CSV_CUSTOM_STAT(WoodProducts, Instances, GetInstanceCount(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, Groups, Runtime->Store.Groups.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, PendingInstances, Runtime->Store.Pending.Num() - Runtime->Store.PendingHead, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, PendingMaintenanceGroups, Runtime->Store.Maintenance.Num() - Runtime->Store.MaintenanceHead, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, NativeBatches, static_cast<int32>(Runtime->Store.NativeBatches), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, TransformUpdates, static_cast<int32>(Runtime->Store.TotalTransformUpdates), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(WoodProducts, PresentationMilliseconds, Runtime->Store.LastApplyMilliseconds, ECsvCustomStatOp::Set);
}
bool UWoodProductPresentationWorldSubsystem::IsTickable() const
{
	return GetWorld() && !GetWorld()->IsNetMode(NM_DedicatedServer) && Runtime && RenderHost && Materials
		&& (!Runtime->Store.Pending.IsEmpty() || !Runtime->Store.Maintenance.IsEmpty());
}
TStatId UWoodProductPresentationWorldSubsystem::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UWoodProductPresentationWorldSubsystem, STATGROUP_Tickables); }
bool UWoodProductPresentationWorldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const { return Type == EWorldType::Game || Type == EWorldType::PIE; }
bool UWoodProductPresentationWorldSubsystem::FindInstance(FWorldEntityId Id, UHierarchicalInstancedStaticMeshComponent*& Component, int32& Index) const
{
	Component = nullptr; Index = INDEX_NONE;
	if (!Runtime) return false;
	const auto* Entry = Runtime->Store.Entries.Find(Id);
	if (!Entry || Entry->Index == INDEX_NONE) return false;
	const auto* Group = Runtime->Store.Groups.Find(Entry->Group);
	if (!Group) return false;
	Component = (*Group)->Component.Get(); Index = Entry->Index;
	return Component != nullptr;
}
uint64 UWoodProductPresentationWorldSubsystem::GetTransformUpdateCount() const { return Runtime ? Runtime->Store.TotalTransformUpdates : 0; }
uint64 UWoodProductPresentationWorldSubsystem::GetInstanceAddCount() const { return Runtime ? Runtime->Store.TotalAdds : 0; }
uint64 UWoodProductPresentationWorldSubsystem::GetInstanceRemoveCount() const { return Runtime ? Runtime->Store.TotalRemoves : 0; }
int32 UWoodProductPresentationWorldSubsystem::GetInstanceCount() const { return static_cast<int32>(GetInstanceAddCount() - GetInstanceRemoveCount()); }
