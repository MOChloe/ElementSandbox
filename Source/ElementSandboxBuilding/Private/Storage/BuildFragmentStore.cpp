#include "Storage/BuildFragmentStore.h"

#include "ElementSandboxBuilding.h"
#include "Entity/BuildFragment.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 InitialFragmentAddressCapacity = 64 * 1024 * 1024;

	SIZE_T GetFragmentByteCount(const int32 Capacity, const int32 Stride)
	{
		check(Capacity >= 0 && Stride > 0);
		check(static_cast<SIZE_T>(Capacity)
			<= TNumericLimits<SIZE_T>::Max() / static_cast<SIZE_T>(Stride));
		return static_cast<SIZE_T>(Capacity) * static_cast<SIZE_T>(Stride);
	}

	int32 SelectFragmentAddressCapacity(const int32 RequiredCapacity)
	{
		check(RequiredCapacity > 0);
		int64 Capacity = InitialFragmentAddressCapacity;
		while (Capacity < RequiredCapacity)
		{
			Capacity *= 2;
			check(Capacity <= MAX_int32);
		}
		return static_cast<int32>(Capacity);
	}
}

FBuildFragmentPool::FIndexPage::FIndexPage()
{
	for (int32& DenseIndex : DenseIndices)
	{
		DenseIndex = INDEX_NONE;
	}
}

FBuildFragmentPool::FBuildFragmentPool(const UScriptStruct& InFragmentType)
	: FragmentType(&InFragmentType)
	, FragmentStride(Align(InFragmentType.GetStructureSize(), InFragmentType.GetMinAlignment()))
	, FragmentAlignment(InFragmentType.GetMinAlignment())
{
	check(FragmentStride > 0);
	check(FragmentAlignment > 0);
}

FBuildFragmentPool::~FBuildFragmentPool()
{
	Empty();
}

bool FBuildFragmentPool::Add(
	const FBuildEntityHandle Entity,
	const FConstStructView Fragment)
{
	check(FragmentType);
	if (Fragment.GetScriptStruct() != FragmentType
		|| !Entity.IsSet()
		|| FindDenseIndex(Entity) != INDEX_NONE)
	{
		return false;
	}

	if (FragmentCount == FragmentCapacity)
	{
		check(FragmentCapacity < MAX_int32);
		Reserve(FragmentCapacity > 0
			? static_cast<int32>(FMath::Min<int64>(
				static_cast<int64>(FragmentCapacity) * 2,
				MAX_int32))
			: 4);
	}

	uint8* Destination = GetFragmentMemory(FragmentCount);
	FragmentType->InitializeStruct(Destination);
	FragmentType->CopyScriptStruct(Destination, Fragment.GetMemory());

	const int32 NewDenseIndex = FragmentCount++;
	Entities.Add(Entity);
	AddDenseIndex(Entity.GetIndex(), NewDenseIndex);

	check(Entities.Num() == FragmentCount);
	return true;
}

bool FBuildFragmentPool::Remove(const FBuildEntityHandle Entity)
{
	const int32 RemoveIndex = FindDenseIndex(Entity);
	if (RemoveIndex == INDEX_NONE)
	{
		return false;
	}

	const int32 LastIndex = FragmentCount - 1;
	check(RemoveIndex >= 0 && RemoveIndex <= LastIndex);

	uint8* RemovedMemory = GetFragmentMemory(RemoveIndex);
	if (RemoveIndex != LastIndex)
	{
		const uint8* LastMemory = GetFragmentMemory(LastIndex);
		FragmentType->DestroyStruct(RemovedMemory);
		FragmentType->InitializeStruct(RemovedMemory);
		FragmentType->CopyScriptStruct(RemovedMemory, LastMemory);

		const FBuildEntityHandle MovedEntity = Entities[LastIndex];
		Entities[RemoveIndex] = MovedEntity;
		UpdateDenseIndex(MovedEntity.GetIndex(), RemoveIndex);
	}

	FragmentType->DestroyStruct(GetFragmentMemory(LastIndex));
	--FragmentCount;
	Entities.Pop(EAllowShrinking::No);
	RemoveDenseIndex(Entity.GetIndex());

	check(Entities.Num() == FragmentCount);
	return true;
}

FConstStructView FBuildFragmentPool::Find(const FBuildEntityHandle Entity) const
{
	const int32 DenseIndex = FindDenseIndex(Entity);
	return DenseIndex != INDEX_NONE
		? FConstStructView(FragmentType, GetFragmentMemory(DenseIndex))
		: FConstStructView();
}

FStructView FBuildFragmentPool::FindMutable(const FBuildEntityHandle Entity)
{
	const int32 DenseIndex = FindDenseIndex(Entity);
	return DenseIndex != INDEX_NONE
		? FStructView(FragmentType, GetFragmentMemory(DenseIndex))
		: FStructView();
}

int32 FBuildFragmentPool::Num() const
{
	return FragmentCount;
}

SIZE_T FBuildFragmentPool::GetAllocatedSize() const
{
	SIZE_T AllocatedSize = FragmentStorage.GetCommittedBytes()
			+ Entities.GetAllocatedSize()
		+ DenseIndexPages.GetAllocatedSize();
	for (const TUniquePtr<FIndexPage>& Page : DenseIndexPages)
	{
		AllocatedSize += Page ? sizeof(FIndexPage) : 0;
	}
	return AllocatedSize;
}

void FBuildFragmentPool::GetEntities(TArray<FBuildEntityHandle>& OutEntities) const
{
	OutEntities.Reset(Entities.Num());
	OutEntities.Append(Entities);
}

int32 FBuildFragmentPool::FindDenseIndex(const FBuildEntityHandle Entity) const
{
	if (!Entity.IsSet())
	{
		return INDEX_NONE;
	}

	const FIndexPage* Page = FindIndexPage(Entity.GetIndex());
	if (!Page)
	{
		return INDEX_NONE;
	}

	const int32 DenseIndex = Page->DenseIndices[Entity.GetIndex() & IndexPageMask];
	return Entities.IsValidIndex(DenseIndex) && Entities[DenseIndex] == Entity
		? DenseIndex
		: INDEX_NONE;
}

void FBuildFragmentPool::AddDenseIndex(
	const int32 EntitySlot,
	const int32 DenseIndex)
{
	FIndexPage& Page = FindOrAddIndexPage(EntitySlot);
	int32& StoredIndex = Page.DenseIndices[EntitySlot & IndexPageMask];
	check(StoredIndex == INDEX_NONE);
	StoredIndex = DenseIndex;
	++Page.OccupiedSlotCount;
}

void FBuildFragmentPool::UpdateDenseIndex(
	const int32 EntitySlot,
	const int32 DenseIndex)
{
	FIndexPage* Page = FindIndexPage(EntitySlot);
	check(Page);
	int32& StoredIndex = Page->DenseIndices[EntitySlot & IndexPageMask];
	check(StoredIndex != INDEX_NONE);
	StoredIndex = DenseIndex;
}

void FBuildFragmentPool::RemoveDenseIndex(const int32 EntitySlot)
{
	const int32 PageIndex = EntitySlot >> IndexPageShift;
	FIndexPage* Page = FindIndexPage(EntitySlot);
	check(Page);
	int32& StoredIndex = Page->DenseIndices[EntitySlot & IndexPageMask];
	check(StoredIndex != INDEX_NONE);
	StoredIndex = INDEX_NONE;
	--Page->OccupiedSlotCount;
	check(Page->OccupiedSlotCount >= 0);
	if (Page->OccupiedSlotCount == 0)
	{
		DenseIndexPages[PageIndex].Reset();
		TrimEmptyIndexPages();
	}
}

FBuildFragmentPool::FIndexPage* FBuildFragmentPool::FindIndexPage(
	const int32 EntitySlot)
{
	if (EntitySlot < 0)
	{
		return nullptr;
	}
	const int32 PageIndex = EntitySlot >> IndexPageShift;
	return DenseIndexPages.IsValidIndex(PageIndex)
		? DenseIndexPages[PageIndex].Get()
		: nullptr;
}

const FBuildFragmentPool::FIndexPage* FBuildFragmentPool::FindIndexPage(
	const int32 EntitySlot) const
{
	if (EntitySlot < 0)
	{
		return nullptr;
	}
	const int32 PageIndex = EntitySlot >> IndexPageShift;
	return DenseIndexPages.IsValidIndex(PageIndex)
		? DenseIndexPages[PageIndex].Get()
		: nullptr;
}

FBuildFragmentPool::FIndexPage& FBuildFragmentPool::FindOrAddIndexPage(
	const int32 EntitySlot)
{
	check(EntitySlot >= 0);
	const int32 PageIndex = EntitySlot >> IndexPageShift;
	if (!DenseIndexPages.IsValidIndex(PageIndex))
	{
		DenseIndexPages.SetNum(PageIndex + 1);
	}
	if (!DenseIndexPages[PageIndex])
	{
		DenseIndexPages[PageIndex] = MakeUnique<FIndexPage>();
	}
	return *DenseIndexPages[PageIndex];
}

void FBuildFragmentPool::TrimEmptyIndexPages()
{
	while (!DenseIndexPages.IsEmpty() && !DenseIndexPages.Last())
	{
		DenseIndexPages.Pop(EAllowShrinking::No);
	}
	if (DenseIndexPages.IsEmpty()
		|| DenseIndexPages.GetSlack() >= DenseIndexPages.Num())
	{
		DenseIndexPages.Shrink();
	}
}

void FBuildFragmentPool::Reserve(const int32 NewCapacity)
{
	if (NewCapacity <= FragmentCapacity)
	{
		return;
	}
	check(FragmentType);
	EnsureFragmentAddressCapacity(NewCapacity);
	FragmentStorage.Commit(GetFragmentByteCount(NewCapacity, FragmentStride));
	FragmentMemory = static_cast<uint8*>(FragmentStorage.GetData());
	FragmentCapacity = NewCapacity;
	Entities.Reserve(NewCapacity);
}

void FBuildFragmentPool::EnsureFragmentAddressCapacity(
	const int32 RequiredCapacity)
{
	if (RequiredCapacity <= FragmentAddressCapacity)
	{
		return;
	}

	const int32 NewAddressCapacity = SelectFragmentAddressCapacity(RequiredCapacity);
	FBuildStableVirtualMemory NewStorage;
	NewStorage.Reserve(
		GetFragmentByteCount(NewAddressCapacity, FragmentStride),
		static_cast<uint32>(FragmentAlignment));
	NewStorage.Commit(GetFragmentByteCount(RequiredCapacity, FragmentStride));
	uint8* NewMemory = static_cast<uint8*>(NewStorage.GetData());
	for (int32 Index = 0; Index < FragmentCount; ++Index)
	{
		uint8* NewFragment = NewMemory + static_cast<SIZE_T>(Index) * FragmentStride;
		FragmentType->InitializeStruct(NewFragment);
		FragmentType->CopyScriptStruct(NewFragment, GetFragmentMemory(Index));
		FragmentType->DestroyStruct(GetFragmentMemory(Index));
	}

	FragmentStorage = MoveTemp(NewStorage);
	FragmentMemory = static_cast<uint8*>(FragmentStorage.GetData());
	FragmentAddressCapacity = NewAddressCapacity;
}

bool FBuildFragmentStore::Reserve(
	const UScriptStruct& FragmentType,
	const int32 NewCapacity)
{
	if (NewCapacity <= 0 || !IsSupportedFragmentType(FragmentType))
	{
		return false;
	}
	FBuildFragmentPoolId PoolId = FindPoolId(FragmentType);
	if (PoolId == InvalidBuildFragmentPoolId)
	{
		PoolId = PoolsById.Add(MakeUnique<FBuildFragmentPool>(FragmentType));
		PoolIdByType.Add(&FragmentType, PoolId);
	}
	check(PoolsById.IsValidIndex(PoolId) && PoolsById[PoolId]);
	PoolsById[PoolId]->Reserve(NewCapacity);
	return true;
}

uint8* FBuildFragmentPool::GetFragmentMemory(const int32 Index)
{
	check(FragmentMemory);
	check(Index >= 0 && Index < FragmentCapacity);
	return FragmentMemory + static_cast<SIZE_T>(Index) * FragmentStride;
}

const uint8* FBuildFragmentPool::GetFragmentMemory(const int32 Index) const
{
	check(FragmentMemory);
	check(Index >= 0 && Index < FragmentCapacity);
	return FragmentMemory + static_cast<SIZE_T>(Index) * FragmentStride;
}

void FBuildFragmentPool::Empty()
{
	if (FragmentType)
	{
		for (int32 Index = 0; Index < FragmentCount; ++Index)
		{
			FragmentType->DestroyStruct(GetFragmentMemory(Index));
		}
	}

	FragmentStorage.Reset();
	FragmentMemory = nullptr;
	FragmentCount = 0;
	FragmentCapacity = 0;
	FragmentAddressCapacity = 0;
	Entities.Empty();
	DenseIndexPages.Empty();
}

bool FBuildFragmentStore::Add(
	const FBuildEntityHandle Entity,
	const FConstStructView Fragment,
	FBuildFragmentPoolId& OutPoolId)
{
	OutPoolId = InvalidBuildFragmentPoolId;
	const UScriptStruct* FragmentType = Fragment.GetScriptStruct();
	if (!Fragment.IsValid() || !FragmentType || !IsSupportedFragmentType(*FragmentType))
	{
		return false;
	}

	FBuildFragmentPoolId PoolId = FindPoolId(*FragmentType);
	if (PoolId == InvalidBuildFragmentPoolId)
	{
		PoolId = PoolsById.Add(MakeUnique<FBuildFragmentPool>(*FragmentType));
		PoolIdByType.Add(FragmentType, PoolId);
	}

	check(PoolsById.IsValidIndex(PoolId) && PoolsById[PoolId]);
	if (!PoolsById[PoolId]->Add(Entity, Fragment))
	{
		return false;
	}
	OutPoolId = PoolId;
	return true;
}

bool FBuildFragmentStore::Remove(
	const FBuildEntityHandle Entity,
	const UScriptStruct& FragmentType,
	FBuildFragmentPoolId& OutPoolId)
{
	OutPoolId = FindPoolId(FragmentType);
	FBuildFragmentPool* Pool = OutPoolId != InvalidBuildFragmentPoolId
		? PoolsById[OutPoolId].Get()
		: nullptr;
	if (!Pool || !Pool->Remove(Entity))
	{
		OutPoolId = InvalidBuildFragmentPoolId;
		return false;
	}
	return true;
}

void FBuildFragmentStore::RemoveEntity(
	const FBuildEntityHandle Entity,
	const TConstArrayView<FBuildFragmentPoolId> OwnedPoolIds)
{
	for (const FBuildFragmentPoolId PoolId : OwnedPoolIds)
	{
		check(PoolsById.IsValidIndex(PoolId) && PoolsById[PoolId]);
		verify(PoolsById[PoolId]->Remove(Entity));
	}
}

FConstStructView FBuildFragmentStore::Find(
	const FBuildEntityHandle Entity,
	const UScriptStruct& FragmentType) const
{
	const FBuildFragmentPool* Pool = FindPool(FragmentType);
	return Pool ? Pool->Find(Entity) : FConstStructView();
}

FStructView FBuildFragmentStore::FindMutable(
	const FBuildEntityHandle Entity,
	const UScriptStruct& FragmentType)
{
	FBuildFragmentPool* Pool = FindPool(FragmentType);
	return Pool ? Pool->FindMutable(Entity) : FStructView();
}

int32 FBuildFragmentStore::GetFragmentCount(const UScriptStruct& FragmentType) const
{
	const FBuildFragmentPool* Pool = FindPool(FragmentType);
	return Pool ? Pool->Num() : 0;
}

SIZE_T FBuildFragmentStore::GetAllocatedSize() const
{
	SIZE_T AllocatedSize = PoolsById.GetAllocatedSize() + PoolIdByType.GetAllocatedSize();
	for (const TUniquePtr<FBuildFragmentPool>& Pool : PoolsById)
	{
		AllocatedSize += Pool ? Pool->GetAllocatedSize() : 0;
	}
	return AllocatedSize;
}

void FBuildFragmentStore::GetEntities(
	const UScriptStruct& FragmentType,
	TArray<FBuildEntityHandle>& OutEntities) const
{
	const FBuildFragmentPool* Pool = FindPool(FragmentType);
	if (Pool)
	{
		Pool->GetEntities(OutEntities);
	}
	else
	{
		OutEntities.Reset();
	}
}

void FBuildFragmentStore::Reset()
{
	PoolIdByType.Reset();
	PoolsById.Reset();
}

bool FBuildFragmentStore::IsSupportedFragmentType(const UScriptStruct& FragmentType)
{
	if (&FragmentType == FBuildFragment::StaticStruct()
		|| !FragmentType.IsChildOf(FBuildFragment::StaticStruct()))
	{
		return false;
	}

	if (const auto* StructOps = FragmentType.GetCppStructOps();
		StructOps && StructOps->HasAddStructReferencedObjects())
	{
		UE_LOG(LogElementSandboxBuilding, Error,
			TEXT("Build Fragment type %s requires custom GC reference collection and cannot be stored."),
			*FragmentType.GetPathName());
		return false;
	}

	// 这里只拒绝需要 Pool 参与反射 GC 的引用。非 UPROPERTY 的 RAII 值成员由
	// UScriptStruct 原生复制/析构管理，并必须在对应 Fragment 的专项测试中验证。
	for (TFieldIterator<FProperty> PropertyIt(&FragmentType); PropertyIt; ++PropertyIt)
	{
		TArray<const FStructProperty*> EncounteredStructProperties;
		if (PropertyIt->ContainsObjectReference(EncounteredStructProperties)
			|| PropertyIt->ContainsWeakObjectReference())
		{
			UE_LOG(LogElementSandboxBuilding, Error,
				TEXT("Build Fragment type %s contains a GC-tracked UObject reference and cannot be stored."),
				*FragmentType.GetPathName());
			return false;
		}
	}

	return true;
}

FBuildFragmentPool* FBuildFragmentStore::FindPool(const UScriptStruct& FragmentType)
{
	const FBuildFragmentPoolId PoolId = FindPoolId(FragmentType);
	return PoolId != InvalidBuildFragmentPoolId ? PoolsById[PoolId].Get() : nullptr;
}

const FBuildFragmentPool* FBuildFragmentStore::FindPool(
	const UScriptStruct& FragmentType) const
{
	const FBuildFragmentPoolId PoolId = FindPoolId(FragmentType);
	return PoolId != InvalidBuildFragmentPoolId ? PoolsById[PoolId].Get() : nullptr;
}

FBuildFragmentPoolId FBuildFragmentStore::FindPoolId(
	const UScriptStruct& FragmentType) const
{
	const FBuildFragmentPoolId* PoolId = PoolIdByType.Find(&FragmentType);
	return PoolId ? *PoolId : InvalidBuildFragmentPoolId;
}
