#pragma once

#include "CoreMinimal.h"
#include "Entity/ElementEntityHandle.h"
#include "Entity/ElementFragment.h"
#include "Entity/WorldEntityId.h"
#include "Templates/UniquePtr.h"
#include <type_traits>

struct ELEMENTSANDBOXSIMULATION_API FElementDirtyPage final
{
	uint64 Epoch = 0;
	TArray<FElementEntityHandle> Entities;

	bool IsEmpty() const { return Entities.IsEmpty(); }
};

struct ELEMENTSANDBOXSIMULATION_API FElementEntityRegistryStats final
{
	int32 EntityCount = 0;
	int32 PendingDirtyCount = 0;
	uint64 DirtyEnqueueCount = 0;
	uint64 DirtyDeduplicatedCount = 0;
};

namespace UE::ElementSandbox::ElementRegistry::Private
{
	template <typename FragmentType>
	const void* FragmentTypeToken()
	{
		static const uint8 Token = 0;
		return &Token;
	}

	class IFragmentPool
	{
	public:
		virtual ~IFragmentPool() = default;
		virtual void RemoveSlot(int32 Slot) = 0;
		virtual bool ContainsSlot(int32 Slot) const = 0;
		virtual SIZE_T GetAllocatedSize() const = 0;
	};

	template <typename FragmentType>
	class TFragmentPool final : public IFragmentPool
	{
	public:
		bool Add(const FElementEntityHandle Entity, const FragmentType& Fragment)
		{
			if (ContainsSlot(Entity.GetSlot())) return false;
			if (Sparse.Num() <= Entity.GetSlot())
			{
				const int32 Previous = Sparse.Num();
				Sparse.SetNum(Entity.GetSlot() + 1);
				for (int32 Index = Previous; Index < Sparse.Num(); ++Index) Sparse[Index] = INDEX_NONE;
			}
			Sparse[Entity.GetSlot()] = Fragments.Num();
			Owners.Add(Entity);
			Fragments.Add(Fragment);
			return true;
		}

		void RemoveSlot(const int32 Slot) override
		{
			if (!ContainsSlot(Slot)) return;
			const int32 Dense = Sparse[Slot];
			const int32 Last = Fragments.Num() - 1;
			if (Dense != Last)
			{
				Fragments[Dense] = MoveTemp(Fragments[Last]);
				Owners[Dense] = Owners[Last];
				Sparse[Owners[Dense].GetSlot()] = Dense;
			}
			Fragments.Pop(EAllowShrinking::No);
			Owners.Pop(EAllowShrinking::No);
			Sparse[Slot] = INDEX_NONE;
		}

		bool ContainsSlot(const int32 Slot) const override
		{
			return Sparse.IsValidIndex(Slot) && Sparse[Slot] != INDEX_NONE;
		}

		FragmentType* Find(const FElementEntityHandle Entity)
		{
			return ContainsSlot(Entity.GetSlot()) ? &Fragments[Sparse[Entity.GetSlot()]] : nullptr;
		}

		const FragmentType* Find(const FElementEntityHandle Entity) const
		{
			return ContainsSlot(Entity.GetSlot()) ? &Fragments[Sparse[Entity.GetSlot()]] : nullptr;
		}

		TConstArrayView<FElementEntityHandle> GetOwners() const { return Owners; }
		TConstArrayView<FragmentType> GetFragments() const { return Fragments; }

		SIZE_T GetAllocatedSize() const override
		{
			return Sparse.GetAllocatedSize() + Owners.GetAllocatedSize() + Fragments.GetAllocatedSize();
		}

	private:
		TArray<int32> Sparse;
		TArray<FElementEntityHandle> Owners;
		TArray<FragmentType> Fragments;
	};
}

template <typename FragmentType>
struct TElementFragmentPoolView final
{
	TConstArrayView<FElementEntityHandle> Entities;
	TConstArrayView<FragmentType> Fragments;
	int32 Num() const { return Fragments.Num(); }
};

/**
 * 专用 Element Authority Registry。Fragment 按具体类型连续存储；外部只能获取 const 视图，
 * 所有写入经过 EditFragment，从写入点直接进入 Next Dirty Page，不扫描布尔标记。
 */
class ELEMENTSANDBOXSIMULATION_API FElementEntityRegistry final
{
public:
	FElementEntityRegistry();
	~FElementEntityRegistry();

	FElementEntityRegistry(const FElementEntityRegistry&) = delete;
	FElementEntityRegistry& operator=(const FElementEntityRegistry&) = delete;

	FElementEntityHandle CreateEntity(FWorldEntityId PersistentId = {});
	bool DestroyEntity(FElementEntityHandle Entity);
	bool IsAlive(FElementEntityHandle Entity) const;
	FWorldEntityId GetPersistentId(FElementEntityHandle Entity) const;
	FElementEntityHandle FindByPersistentId(FWorldEntityId PersistentId) const;
	uint64 GetEntityRevision(FElementEntityHandle Entity) const;

	template <typename FragmentType>
	bool AddFragment(const FElementEntityHandle Entity, const FragmentType& Fragment)
	{
		static_assert(std::is_base_of_v<FElementFragment, FragmentType>);
		if (!IsAlive(Entity)) return false;
		auto* Pool = FindOrAddPool<FragmentType>();
		if (!Pool->Add(Entity, Fragment)) return false;
		AdvanceRevision(Entity);
		MarkDirty(Entity);
		return true;
	}

	template <typename FragmentType>
	bool RemoveFragment(const FElementEntityHandle Entity)
	{
		static_assert(std::is_base_of_v<FElementFragment, FragmentType>);
		if (!IsAlive(Entity)) return false;
		auto* Pool = FindPool<FragmentType>();
		if (!Pool || !Pool->ContainsSlot(Entity.GetSlot())) return false;
		Pool->RemoveSlot(Entity.GetSlot());
		AdvanceRevision(Entity);
		MarkDirty(Entity);
		return true;
	}

	template <typename FragmentType, typename EditFunction>
	bool EditFragment(const FElementEntityHandle Entity, EditFunction&& Edit)
	{
		static_assert(std::is_base_of_v<FElementFragment, FragmentType>);
		if (!IsAlive(Entity)) return false;
		auto* Pool = FindPool<FragmentType>();
		FragmentType* Fragment = Pool ? Pool->Find(Entity) : nullptr;
		if (!Fragment) return false;
		Edit(*Fragment);
		if constexpr (std::is_base_of_v<FElementInfluenceFragment, FragmentType>)
		{
			Fragment->Revision = Fragment->Revision == MAX_uint64 ? 1 : Fragment->Revision + 1;
		}
		AdvanceRevision(Entity);
		MarkDirty(Entity);
		return true;
	}

	template <typename FragmentType>
	const FragmentType* FindFragment(const FElementEntityHandle Entity) const
	{
		static_assert(std::is_base_of_v<FElementFragment, FragmentType>);
		if (!IsAlive(Entity)) return nullptr;
		const auto* Pool = FindPool<FragmentType>();
		return Pool ? Pool->Find(Entity) : nullptr;
	}

	template <typename FragmentType>
	TElementFragmentPoolView<FragmentType> GetFragmentPoolView() const
	{
		static_assert(std::is_base_of_v<FElementFragment, FragmentType>);
		const auto* Pool = FindPool<FragmentType>();
		return Pool ? TElementFragmentPoolView<FragmentType>{Pool->GetOwners(), Pool->GetFragments()}
			: TElementFragmentPoolView<FragmentType>();
	}

	/** Current 仍在处理时只交换 Next；新写入永远不会进入已封闭页。 */
	bool SealDirtyPage(FElementDirtyPage& OutPage);
	int32 GetPendingDirtyCount() const { return NextDirty.Num(); }
	FElementEntityRegistryStats GetStats() const;
	SIZE_T GetAllocatedSize() const;

private:
	struct FEntityRecord final
	{
		uint32 Generation = 1;
		uint64 Revision = 0;
		FWorldEntityId PersistentId;
		bool bAlive = false;
	};

	template <typename FragmentType>
	UE::ElementSandbox::ElementRegistry::Private::TFragmentPool<FragmentType>* FindOrAddPool()
	{
		using namespace UE::ElementSandbox::ElementRegistry::Private;
		const void* Token = FragmentTypeToken<FragmentType>();
		TUniquePtr<IFragmentPool>& Base = Pools.FindOrAdd(Token);
		if (!Base) Base = MakeUnique<TFragmentPool<FragmentType>>();
		return static_cast<TFragmentPool<FragmentType>*>(Base.Get());
	}

	template <typename FragmentType>
	UE::ElementSandbox::ElementRegistry::Private::TFragmentPool<FragmentType>* FindPool()
	{
		using namespace UE::ElementSandbox::ElementRegistry::Private;
		TUniquePtr<IFragmentPool>* Base = Pools.Find(FragmentTypeToken<FragmentType>());
		return Base ? static_cast<TFragmentPool<FragmentType>*>(Base->Get()) : nullptr;
	}

	template <typename FragmentType>
	const UE::ElementSandbox::ElementRegistry::Private::TFragmentPool<FragmentType>* FindPool() const
	{
		using namespace UE::ElementSandbox::ElementRegistry::Private;
		const TUniquePtr<IFragmentPool>* Base = Pools.Find(FragmentTypeToken<FragmentType>());
		return Base ? static_cast<const TFragmentPool<FragmentType>*>(Base->Get()) : nullptr;
	}

	void AdvanceRevision(FElementEntityHandle Entity);
	void MarkDirty(FElementEntityHandle Entity);

	uint32 RegistryId = 0;
	uint64 NextDirtyEpoch = 1;
	TArray<FEntityRecord> Records;
	TArray<int32> FreeSlots;
	TMap<FWorldEntityId, FElementEntityHandle> EntityByPersistentId;
	TMap<const void*, TUniquePtr<UE::ElementSandbox::ElementRegistry::Private::IFragmentPool>> Pools;
	TSet<FElementEntityHandle> NextDirty;
	uint64 DirtyEnqueueCount = 0;
	uint64 DirtyDeduplicatedCount = 0;
};
