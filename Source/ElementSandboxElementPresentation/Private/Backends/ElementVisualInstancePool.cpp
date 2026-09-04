#include "Backends/ElementVisualInstancePool.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

struct FElementVisualInstancePool::FPage final
{
	FName DefinitionId = NAME_None;
	EElementVisualInstanceBackend Backend = EElementVisualInstanceBackend::Hierarchical;
	UInstancedStaticMeshComponent* Component = nullptr;
	TArray<FElementVisualKey> KeysByInstance;
	bool bSpare = false;
};

FElementVisualInstancePool::FElementVisualInstancePool(
	UWorld& InWorld,
	const FElementPresentationConfig& InConfig)
	: World(&InWorld)
	, Config(InConfig)
{
	check(Config.IsValid());
}

FElementVisualInstancePool::~FElementVisualInstancePool()
{
	Reset();
}

bool FElementVisualInstancePool::EnsureHost()
{
	check(IsInGameThread());
	if (Host.IsValid())
	{
		return true;
	}
	if (!World || World->IsNetMode(NM_DedicatedServer))
	{
		return false;
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* NewHost = World->SpawnActor<AActor>(Parameters);
	if (!NewHost)
	{
		return false;
	}
	NewHost->PrimaryActorTick.bCanEverTick = false;
	NewHost->SetReplicates(false);
	NewHost->SetActorEnableCollision(false);
	USceneComponent* Root = NewObject<USceneComponent>(
		NewHost,
		MakeUniqueObjectName(NewHost, USceneComponent::StaticClass(), TEXT("ElementVisualRoot")));
	if (!Root)
	{
		NewHost->Destroy();
		return false;
	}
	NewHost->AddInstanceComponent(Root);
	NewHost->SetRootComponent(Root);
	Root->RegisterComponent();
	Host = NewHost;
	return true;
}

bool FElementVisualInstancePool::ConfigurePage(
	FPage& Page,
	const FElementVisualDefinition& Definition)
{
	UStaticMesh* Mesh = Definition.StaticMesh.Get();
	if (!Page.Component || !Mesh)
	{
		return false;
	}
	Page.Component->ClearInstances();
	Page.Component->SetStaticMesh(Mesh);
	Page.Component->SetMaterial(0, Definition.MaterialOverride.Get());
	Page.Component->SetNumCustomDataFloats(Definition.CustomDataFloatCount);
	Page.DefinitionId = Definition.DefinitionId;
	Page.Backend = Definition.Backend;
	Page.KeysByInstance.Reset();
	Page.bSpare = false;
	return true;
}

int32 FElementVisualInstancePool::AllocatePage(const FElementVisualDefinition& Definition)
{
	if (!EnsureHost())
	{
		return INDEX_NONE;
	}
	AActor* Owner = Host.Get();
	UInstancedStaticMeshComponent* Component = nullptr;
	if (Definition.Backend == EElementVisualInstanceBackend::Hierarchical)
	{
		Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(
			Owner,
			MakeUniqueObjectName(Owner, UHierarchicalInstancedStaticMeshComponent::StaticClass(),
				TEXT("ElementVisualHISM")));
	}
	else
	{
		Component = NewObject<UInstancedStaticMeshComponent>(
			Owner,
			MakeUniqueObjectName(Owner, UInstancedStaticMeshComponent::StaticClass(), TEXT("ElementVisualISM")));
	}
	if (!Component)
	{
		return INDEX_NONE;
	}
	Component->SetupAttachment(Owner->GetRootComponent());
	Component->SetRemoveSwap();
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bCastDynamicShadow = false;
	Component->bCastStaticShadow = false;
	Component->SetReceivesDecals(false);
	Owner->AddInstanceComponent(Component);
	Component->RegisterComponent();

	TUniquePtr<FPage> NewPage = MakeUnique<FPage>();
	NewPage->Component = Component;
	if (!ConfigurePage(*NewPage, Definition))
	{
		Owner->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		return INDEX_NONE;
	}
	int32 PageIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Pages.Num(); ++Index)
	{
		if (!Pages[Index])
		{
			PageIndex = Index;
			Pages[Index] = MoveTemp(NewPage);
			break;
		}
	}
	if (PageIndex == INDEX_NONE)
	{
		PageIndex = Pages.Add(MoveTemp(NewPage));
	}
	++PageAllocateCount;
	return PageIndex;
}

int32 FElementVisualInstancePool::AcquirePage(const FElementVisualDefinition& Definition)
{
	for (int32 Index = 0; Index < Pages.Num(); ++Index)
	{
		const TUniquePtr<FPage>& Page = Pages[Index];
		if (Page && !Page->bSpare && Page->DefinitionId == Definition.DefinitionId
			&& Page->Backend == Definition.Backend
			&& Page->KeysByInstance.Num() < Config.InstancesPerPage)
		{
			return Index;
		}
	}
	for (int32 Index = 0; Index < Pages.Num(); ++Index)
	{
		TUniquePtr<FPage>& Page = Pages[Index];
		if (Page && Page->bSpare && Page->Backend == Definition.Backend)
		{
			if (!ConfigurePage(*Page, Definition))
			{
				DestroyPage(Index);
				break;
			}
			++PageReuseCount;
			return Index;
		}
	}
	return AllocatePage(Definition);
}

bool FElementVisualInstancePool::ApplyCustomData(
	UInstancedStaticMeshComponent& Component,
	const int32 InstanceIndex,
	const FElementVisualDescriptor& Descriptor,
	const int32 CustomDataFloatCount)
{
	const float Values[5] = {
		Descriptor.Color.R,
		Descriptor.Color.G,
		Descriptor.Color.B,
		Descriptor.Color.A,
		Descriptor.Intensity
	};
	for (int32 Index = 0; Index < CustomDataFloatCount; ++Index)
	{
		if (!Component.SetCustomDataValue(InstanceIndex, Index, Values[Index], false))
		{
			return false;
		}
	}
	return true;
}

bool FElementVisualInstancePool::AddToPage(
	const int32 PageIndex,
	const FElementVisualDescriptor& Descriptor,
	const FElementVisualDefinition& Definition)
{
	if (!Pages.IsValidIndex(PageIndex) || !Pages[PageIndex] || Locations.Contains(Descriptor.Key))
	{
		return false;
	}
	FPage& Page = *Pages[PageIndex];
	if (Page.bSpare || Page.DefinitionId != Definition.DefinitionId || Page.Backend != Definition.Backend
		|| !Page.Component || Page.KeysByInstance.Num() >= Config.InstancesPerPage)
	{
		return false;
	}
	const int32 InstanceIndex = Page.Component->AddInstance(Descriptor.WorldTransform, true);
	if (InstanceIndex != Page.KeysByInstance.Num())
	{
		if (InstanceIndex != INDEX_NONE)
		{
			Page.Component->RemoveInstance(InstanceIndex);
		}
		return false;
	}
	if (!ApplyCustomData(*Page.Component, InstanceIndex, Descriptor, Definition.CustomDataFloatCount))
	{
		Page.Component->RemoveInstance(InstanceIndex);
		return false;
	}
	Page.KeysByInstance.Add(Descriptor.Key);
	Locations.Add(Descriptor.Key, {PageIndex, InstanceIndex});
	Page.Component->MarkRenderStateDirty();
	return true;
}

bool FElementVisualInstancePool::UpdateInPage(
	const FLocation& Location,
	const FElementVisualDescriptor& Descriptor,
	const FElementVisualDefinition& Definition)
{
	if (!Pages.IsValidIndex(Location.PageIndex) || !Pages[Location.PageIndex])
	{
		return false;
	}
	FPage& Page = *Pages[Location.PageIndex];
	if (Page.bSpare || Page.DefinitionId != Definition.DefinitionId || Page.Backend != Definition.Backend
		|| !Page.Component || !Page.KeysByInstance.IsValidIndex(Location.InstanceIndex)
		|| Page.KeysByInstance[Location.InstanceIndex] != Descriptor.Key)
	{
		return false;
	}
	if (!Page.Component->UpdateInstanceTransform(
		Location.InstanceIndex, Descriptor.WorldTransform, true, false, true)
		|| !ApplyCustomData(*Page.Component, Location.InstanceIndex, Descriptor,
			Definition.CustomDataFloatCount))
	{
		return false;
	}
	Page.Component->MarkRenderStateDirty();
	return true;
}

bool FElementVisualInstancePool::Upsert(
	const FElementVisualDescriptor& Descriptor,
	const FElementVisualDefinition& Definition)
{
	check(IsInGameThread());
	if (!Descriptor.IsValid() || !Definition.IsValid()
		|| Descriptor.VisualDefinitionId != Definition.DefinitionId)
	{
		return false;
	}
	if (const FLocation* Existing = Locations.Find(Descriptor.Key))
	{
		if (Pages.IsValidIndex(Existing->PageIndex) && Pages[Existing->PageIndex]
			&& Pages[Existing->PageIndex]->DefinitionId == Definition.DefinitionId
			&& Pages[Existing->PageIndex]->Backend == Definition.Backend)
		{
			return UpdateInPage(*Existing, Descriptor, Definition);
		}
		if (!Remove(Descriptor.Key))
		{
			return false;
		}
	}
	const int32 PageIndex = AcquirePage(Definition);
	return PageIndex != INDEX_NONE && AddToPage(PageIndex, Descriptor, Definition);
}

bool FElementVisualInstancePool::Remove(const FElementVisualKey& Key)
{
	check(IsInGameThread());
	const FLocation* Existing = Locations.Find(Key);
	if (!Existing || !Pages.IsValidIndex(Existing->PageIndex) || !Pages[Existing->PageIndex])
	{
		return false;
	}
	const int32 PageIndex = Existing->PageIndex;
	const int32 InstanceIndex = Existing->InstanceIndex;
	FPage& Page = *Pages[PageIndex];
	if (!Page.Component || !Page.KeysByInstance.IsValidIndex(InstanceIndex)
		|| Page.KeysByInstance[InstanceIndex] != Key)
	{
		return false;
	}
	const int32 LastIndex = Page.KeysByInstance.Num() - 1;
	const FElementVisualKey MovedKey = Page.KeysByInstance[LastIndex];
	if (!Page.Component->RemoveInstance(InstanceIndex))
	{
		return false;
	}
	if (InstanceIndex != LastIndex)
	{
		Page.KeysByInstance[InstanceIndex] = MovedKey;
		FLocation& MovedLocation = Locations.FindChecked(MovedKey);
		MovedLocation.InstanceIndex = InstanceIndex;
	}
	Page.KeysByInstance.Pop(EAllowShrinking::No);
	Locations.Remove(Key);
	Page.Component->MarkRenderStateDirty();
	if (Page.KeysByInstance.IsEmpty())
	{
		RetireEmptyPage(PageIndex);
	}
	return true;
}

void FElementVisualInstancePool::RetireEmptyPage(const int32 PageIndex)
{
	if (!Pages.IsValidIndex(PageIndex) || !Pages[PageIndex])
	{
		return;
	}
	FPage& Page = *Pages[PageIndex];
	Page.Component->ClearInstances();
	Page.DefinitionId = NAME_None;
	Page.bSpare = true;
	if (CountSparePages(Page.Backend) > Config.MaxSparePagesPerBackend)
	{
		DestroyPage(PageIndex);
	}
}

int32 FElementVisualInstancePool::CountSparePages(const EElementVisualInstanceBackend Backend) const
{
	int32 Count = 0;
	for (const TUniquePtr<FPage>& Page : Pages)
	{
		Count += Page && Page->bSpare && Page->Backend == Backend ? 1 : 0;
	}
	return Count;
}

void FElementVisualInstancePool::DestroyPage(const int32 PageIndex)
{
	if (!Pages.IsValidIndex(PageIndex) || !Pages[PageIndex])
	{
		return;
	}
	FPage& Page = *Pages[PageIndex];
	if (Page.Component)
	{
		if (AActor* Owner = Host.Get())
		{
			Owner->RemoveInstanceComponent(Page.Component);
		}
		Page.Component->DestroyComponent();
	}
	Pages[PageIndex].Reset();
}

bool FElementVisualInstancePool::Contains(const FElementVisualKey& Key) const
{
	return Locations.Contains(Key);
}

void FElementVisualInstancePool::Reset()
{
	check(IsInGameThread());
	Locations.Reset();
	for (int32 Index = 0; Index < Pages.Num(); ++Index)
	{
		DestroyPage(Index);
	}
	Pages.Reset();
	if (AActor* Owner = Host.Get())
	{
		if (!World || !World->IsBeingCleanedUp())
		{
			Owner->Destroy();
		}
	}
	Host.Reset();
}

void FElementVisualInstancePool::AppendStats(FElementPresentationStats& InOutStats) const
{
	InOutStats.ActivePageCount = 0;
	InOutStats.SparePageCount = 0;
	InOutStats.HISMComponentCount = 0;
	InOutStats.ISMComponentCount = 0;
	for (const TUniquePtr<FPage>& Page : Pages)
	{
		if (!Page)
		{
			continue;
		}
		if (Page->bSpare)
		{
			++InOutStats.SparePageCount;
		}
		else
		{
			++InOutStats.ActivePageCount;
		}
		if (Page->Backend == EElementVisualInstanceBackend::Hierarchical)
		{
			++InOutStats.HISMComponentCount;
		}
		else
		{
			++InOutStats.ISMComponentCount;
		}
	}
	InOutStats.PoolPageAllocateCount = PageAllocateCount;
	InOutStats.PoolPageReuseCount = PageReuseCount;
}
