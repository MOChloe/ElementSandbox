#include "Storage/BuildStableArrayAllocator.h"

#include "HAL/UnrealMemory.h"

namespace
{
	constexpr SIZE_T CommitChunkBytes = 64ull * 1024ull;
	constexpr SIZE_T InitialArrayVirtualReservationBytes = 8ull * 1024ull * 1024ull * 1024ull;

	SIZE_T CheckedByteCount(const int32 Count, const SIZE_T BytesPerElement)
	{
		check(Count >= 0 && BytesPerElement > 0);
		check(static_cast<SIZE_T>(Count) <= TNumericLimits<SIZE_T>::Max() / BytesPerElement);
		return static_cast<SIZE_T>(Count) * BytesPerElement;
	}

	SIZE_T SelectReservationBytes(const SIZE_T RequiredBytes)
	{
		SIZE_T ReservationBytes = InitialArrayVirtualReservationBytes;
		while (ReservationBytes < RequiredBytes)
		{
			check(ReservationBytes <= TNumericLimits<SIZE_T>::Max() / 2);
			ReservationBytes *= 2;
		}
		return ReservationBytes;
	}
}

FBuildStableVirtualMemory::~FBuildStableVirtualMemory()
{
	Reset();
}

FBuildStableVirtualMemory::FBuildStableVirtualMemory(
	FBuildStableVirtualMemory&& Other) noexcept
{
	MoveFrom(Other);
}

FBuildStableVirtualMemory& FBuildStableVirtualMemory::operator=(
	FBuildStableVirtualMemory&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		MoveFrom(Other);
	}
	return *this;
}

void FBuildStableVirtualMemory::Reserve(
	const SIZE_T MinimumReservedBytes,
	const uint32 Alignment)
{
	check(MinimumReservedBytes > 0);
	check(!VirtualMemory.GetVirtualPointer());
	check(FPlatformMemory::CanOverallocateVirtualMemory());

	const SIZE_T VirtualAlignment = FPlatformMemory::FPlatformVirtualMemoryBlock::GetVirtualSizeAlignment();
	const SIZE_T ReservationAlignment = FMath::Max<SIZE_T>(VirtualAlignment, CommitChunkBytes);
	ReservedBytes = Align(MinimumReservedBytes, ReservationAlignment);
	VirtualMemory = FPlatformMemory::FPlatformVirtualMemoryBlock::AllocateVirtual(
		ReservedBytes,
		FMath::Max<SIZE_T>(Alignment, VirtualAlignment));
	check(VirtualMemory.GetVirtualPointer());
	check(IsAligned(VirtualMemory.GetVirtualPointer(), Alignment));
}

void FBuildStableVirtualMemory::Commit(const SIZE_T RequiredBytes)
{
	if (RequiredBytes <= CommittedBytes)
	{
		return;
	}
	check(VirtualMemory.GetVirtualPointer());
	const SIZE_T CommitAlignment = FMath::Max<SIZE_T>(
		FPlatformMemory::FPlatformVirtualMemoryBlock::GetCommitAlignment(),
		CommitChunkBytes);
	const SIZE_T NewCommittedBytes = Align(RequiredBytes, CommitAlignment);
	check(NewCommittedBytes <= ReservedBytes);
	VirtualMemory.Commit(
		CommittedBytes,
		NewCommittedBytes - CommittedBytes);
	CommittedBytes = NewCommittedBytes;
}

void FBuildStableVirtualMemory::Reset()
{
	VirtualMemory.FreeVirtual();
	ReservedBytes = 0;
	CommittedBytes = 0;
}

void FBuildStableVirtualMemory::MoveFrom(FBuildStableVirtualMemory& Other)
{
	VirtualMemory = Other.VirtualMemory;
	ReservedBytes = Other.ReservedBytes;
	CommittedBytes = Other.CommittedBytes;
	Other.VirtualMemory = FPlatformMemory::FPlatformVirtualMemoryBlock();
	Other.ReservedBytes = 0;
	Other.CommittedBytes = 0;
}

void FBuildStableArrayAllocator::ForAnyElementType::MoveToEmpty(
	ForAnyElementType& Other)
{
	checkSlow(this != &Other);
	Storage = MoveTemp(Other.Storage);
}

void FBuildStableArrayAllocator::ForAnyElementType::ResizeAllocation(
	const SizeType CurrentNum,
	const SizeType NewMax,
	const SIZE_T NumBytesPerElement)
{
	ResizeAllocationInternal(
		CurrentNum,
		NewMax,
		NumBytesPerElement,
		DEFAULT_ALIGNMENT);
}

void FBuildStableArrayAllocator::ForAnyElementType::ResizeAllocation(
	const SizeType CurrentNum,
	const SizeType NewMax,
	const SIZE_T NumBytesPerElement,
	const uint32 AlignmentOfElement)
{
	ResizeAllocationInternal(
		CurrentNum,
		NewMax,
		NumBytesPerElement,
		AlignmentOfElement);
}

void FBuildStableArrayAllocator::ForAnyElementType::ResizeAllocationInternal(
	const SizeType CurrentNum,
	const SizeType NewMax,
	const SIZE_T NumBytesPerElement,
	const uint32 AlignmentOfElement)
{
	check(CurrentNum >= 0 && NewMax >= 0 && NumBytesPerElement > 0);
	if (NewMax == 0)
	{
		Storage.Reset();
		return;
	}

	const SIZE_T RequiredBytes = CheckedByteCount(NewMax, NumBytesPerElement);
	if (!Storage.GetData())
	{
		Storage.Reserve(
			SelectReservationBytes(RequiredBytes),
			AlignmentOfElement);
	}
	else if (RequiredBytes > Storage.GetReservedBytes())
	{
		FBuildStableVirtualMemory NewStorage;
		NewStorage.Reserve(
			SelectReservationBytes(RequiredBytes),
			AlignmentOfElement);
		NewStorage.Commit(RequiredBytes);
		FMemory::Memcpy(
			NewStorage.GetData(),
			Storage.GetData(),
			CheckedByteCount(CurrentNum, NumBytesPerElement));
		Storage = MoveTemp(NewStorage);
		return;
	}
	Storage.Commit(RequiredBytes);
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackReserve(
	const SizeType NewMax,
	const SIZE_T NumBytesPerElement) const
{
	return DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, false);
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackReserve(
	const SizeType NewMax,
	const SIZE_T NumBytesPerElement,
	const uint32 AlignmentOfElement) const
{
	return DefaultCalculateSlackReserve(
		NewMax,
		NumBytesPerElement,
		false,
		AlignmentOfElement);
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackShrink(
	const SizeType NewMax,
	const SizeType CurrentMax,
	const SIZE_T NumBytesPerElement) const
{
	return NewMax == 0 ? 0 : CurrentMax;
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackShrink(
	const SizeType NewMax,
	const SizeType CurrentMax,
	const SIZE_T NumBytesPerElement,
	const uint32 AlignmentOfElement) const
{
	return NewMax == 0 ? 0 : CurrentMax;
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackGrow(
	const SizeType NewMax,
	const SizeType CurrentMax,
	const SIZE_T NumBytesPerElement) const
{
	return DefaultCalculateSlackGrow(
		NewMax,
		CurrentMax,
		NumBytesPerElement,
		false);
}

FBuildStableArrayAllocator::SizeType
FBuildStableArrayAllocator::ForAnyElementType::CalculateSlackGrow(
	const SizeType NewMax,
	const SizeType CurrentMax,
	const SIZE_T NumBytesPerElement,
	const uint32 AlignmentOfElement) const
{
	return DefaultCalculateSlackGrow(
		NewMax,
		CurrentMax,
		NumBytesPerElement,
		false,
		AlignmentOfElement);
}

SIZE_T FBuildStableArrayAllocator::ForAnyElementType::GetAllocatedSize(
	const SizeType CurrentMax,
	const SIZE_T NumBytesPerElement) const
{
	return Storage.GetCommittedBytes();
}
