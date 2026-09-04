#pragma once

#include "Containers/ContainerAllocationPolicies.h"
#include "HAL/PlatformMemory.h"

/**
 * 在 64-bit 平台预留连续虚拟地址、按需提交物理页的存储块。
 * 扩大已提交范围不会改变首地址；只有超过整段地址预留时才需要更换 Block。
 */
class FBuildStableVirtualMemory final
{
public:
	FBuildStableVirtualMemory() = default;
	~FBuildStableVirtualMemory();

	FBuildStableVirtualMemory(const FBuildStableVirtualMemory&) = delete;
	FBuildStableVirtualMemory& operator=(const FBuildStableVirtualMemory&) = delete;
	FBuildStableVirtualMemory(FBuildStableVirtualMemory&& Other) noexcept;
	FBuildStableVirtualMemory& operator=(FBuildStableVirtualMemory&& Other) noexcept;

	void Reserve(SIZE_T MinimumReservedBytes, uint32 Alignment = DEFAULT_ALIGNMENT);
	void Commit(SIZE_T RequiredBytes);
	void Reset();

	void* GetData() const { return VirtualMemory.GetVirtualPointer(); }
	SIZE_T GetReservedBytes() const { return ReservedBytes; }
	SIZE_T GetCommittedBytes() const { return CommittedBytes; }

private:
	void MoveFrom(FBuildStableVirtualMemory& Other);

	FPlatformMemory::FPlatformVirtualMemoryBlock VirtualMemory;
	SIZE_T ReservedBytes = 0;
	SIZE_T CommittedBytes = 0;
};

/**
 * 保留 TArray 的连续访问/API，但把 Realloc 改为固定虚拟地址内的增量 Commit。
 * 8 GiB 是地址空间而非物理内存；实际 CPU 内存仍只随 ArrayMax 按页增长。
 */
class FBuildStableArrayAllocator
{
public:
	using SizeType = int32;

	enum { NeedsElementType = false };
	enum { RequireRangeCheck = true };

	class ForAnyElementType
	{
	public:
		ForAnyElementType() = default;
		~ForAnyElementType() = default;

		ForAnyElementType(const ForAnyElementType&) = delete;
		ForAnyElementType& operator=(const ForAnyElementType&) = delete;

		void MoveToEmpty(ForAnyElementType& Other);
		FScriptContainerElement* GetAllocation() const
		{
			return static_cast<FScriptContainerElement*>(Storage.GetData());
		}

		void ResizeAllocation(
			SizeType CurrentNum,
			SizeType NewMax,
			SIZE_T NumBytesPerElement);
		void ResizeAllocation(
			SizeType CurrentNum,
			SizeType NewMax,
			SIZE_T NumBytesPerElement,
			uint32 AlignmentOfElement);

		SizeType CalculateSlackReserve(
			SizeType NewMax,
			SIZE_T NumBytesPerElement) const;
		SizeType CalculateSlackReserve(
			SizeType NewMax,
			SIZE_T NumBytesPerElement,
			uint32 AlignmentOfElement) const;
		SizeType CalculateSlackShrink(
			SizeType NewMax,
			SizeType CurrentMax,
			SIZE_T NumBytesPerElement) const;
		SizeType CalculateSlackShrink(
			SizeType NewMax,
			SizeType CurrentMax,
			SIZE_T NumBytesPerElement,
			uint32 AlignmentOfElement) const;
		SizeType CalculateSlackGrow(
			SizeType NewMax,
			SizeType CurrentMax,
			SIZE_T NumBytesPerElement) const;
		SizeType CalculateSlackGrow(
			SizeType NewMax,
			SizeType CurrentMax,
			SIZE_T NumBytesPerElement,
			uint32 AlignmentOfElement) const;

		SIZE_T GetAllocatedSize(
			SizeType CurrentMax,
			SIZE_T NumBytesPerElement) const;
		bool HasAllocation() const { return Storage.GetData() != nullptr; }
		SizeType GetInitialCapacity() const { return 0; }

	private:
		void ResizeAllocationInternal(
			SizeType CurrentNum,
			SizeType NewMax,
			SIZE_T NumBytesPerElement,
			uint32 AlignmentOfElement);

		FBuildStableVirtualMemory Storage;
	};

	template <typename ElementType>
	class ForElementType : public ForAnyElementType
	{
	public:
		ElementType* GetAllocation() const
		{
			return reinterpret_cast<ElementType*>(ForAnyElementType::GetAllocation());
		}
	};
};

template <>
struct TAllocatorTraits<FBuildStableArrayAllocator>
	: TAllocatorTraitsBase<FBuildStableArrayAllocator>
{
	enum { SupportsElementAlignment = true };
};
