#include "WorldSeed/MillionSettlementSeedLayout.h"

#include "WorldSeed/MillionBuildingRecipe.h"

namespace UE::ElementSandbox::WorldSeed
{
	namespace
	{
		constexpr double LotSpacingCentimeters = 4000.0;
		constexpr double HalfLotCentimeters = LotSpacingCentimeters * 0.5;
		constexpr double VillageGreenDepthCentimeters = 3000.0;
		constexpr double MainPathHalfWidthCentimeters = 1800.0;
		constexpr int32 LotsPerLane = 10;
		constexpr double LaneExtraCentimeters = 1500.0;
		constexpr int32 PathDriftSegmentRows = 16;
		constexpr double MaximumPathDriftCentimeters = 800.0;
		constexpr double MaximumLotJitterCentimeters = 220.0;
		constexpr double MaximumYawJitterDegrees = 6.0;

		uint64 MixCityHash(uint64 Value)
		{
			Value += 0x9e3779b97f4a7c15ull;
			Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
			Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
			return Value ^ (Value >> 31);
		}

		bool TryResolveLayoutAddress(
			const uint32 LayoutIndex, int32& OutRow, int32& OutPairIndex, int32& OutSideSign)
		{
			if (LayoutIndex >= static_cast<uint32>(CompleteStructureCount))
			{
				return false;
			}
			OutRow = static_cast<int32>(LayoutIndex / LayoutColumns);
			const int32 Column = static_cast<int32>(LayoutIndex % LayoutColumns);
			OutPairIndex = Column / 2;
			OutSideSign = Column % 2 == 0 ? -1 : 1;
			return true;
		}

		uint64 GetCityCellHash(const int32 Seed, const int32 Row, const int32 PairIndex, const int32 SideSign)
		{
			uint64 Value = static_cast<uint32>(Seed);
			Value ^= static_cast<uint64>(static_cast<uint32>(Row)) << 32;
			Value ^= static_cast<uint32>(PairIndex) * 0x9e3779b9u;
			Value ^= SideSign > 0 ? 0xd1b54a32d192ed03ull : 0x94d049bb133111ebull;
			return MixCityHash(Value);
		}

		double HashToSignedRange(const uint64 Hash, const double MaximumMagnitude)
		{
			const double Unit = static_cast<double>(Hash & 0xffffu) / 65535.0;
			return (Unit * 2.0 - 1.0) * MaximumMagnitude;
		}

		double ResolvePathCenterOffset(const int32 Seed, const int32 Row)
		{
			const int32 Segment = Row / PathDriftSegmentRows;
			const double SegmentAlpha = static_cast<double>(Row % PathDriftSegmentRows) / PathDriftSegmentRows;
			const double SmoothAlpha = SegmentAlpha * SegmentAlpha * (3.0 - 2.0 * SegmentAlpha);
			const uint64 HashSeed = static_cast<uint32>(Seed);
			const double Start = HashToSignedRange(
				MixCityHash(HashSeed ^ (static_cast<uint64>(Segment) * 0x632be59bd9b4e019ull)),
				MaximumPathDriftCentimeters);
			const double End = HashToSignedRange(
				MixCityHash(HashSeed ^ (static_cast<uint64>(Segment + 1) * 0x632be59bd9b4e019ull)),
				MaximumPathDriftCentimeters);
			return FMath::Lerp(Start, End, SmoothAlpha);
		}
	}

	bool BuildRecipeCatalog(UObject* Outer, TArray<UCityBuildingRecipe*>& OutRecipes, FString& OutError)
	{
		OutRecipes.Reset();
		OutError.Reset();
		if (!Outer)
		{
			OutError = TEXT("配方 Outer 为空。");
			return false;
		}
		for (const ECityBuildingArchetype Archetype : GetDefaultCityBuildingArchetypes())
		{
			UCityBuildingRecipe* Recipe = NewObject<UCityBuildingRecipe>(Outer);
			if (!Recipe || !Recipe->Initialize(Archetype) || Recipe->GetPieces().IsEmpty())
			{
				OutRecipes.Reset();
				OutError = FString::Printf(TEXT("无法创建离线建筑配方：%s"), *GetCityBuildingRecipeId(Archetype).ToString());
				return false;
			}
			OutRecipes.Add(Recipe);
		}
		if (OutRecipes.Num() != 12)
		{
			OutRecipes.Reset();
			OutError = TEXT("离线种子生成器要求恰好 12 套配方。");
			return false;
		}
		return true;
	}

	uint8 ResolveArchetypeIndex(const uint32 LayoutIndex, const int32 Seed)
	{
		int32 Row = 0;
		int32 PairIndex = 0;
		int32 SideSign = 0;
		if (!TryResolveLayoutAddress(LayoutIndex, Row, PairIndex, SideSign))
		{
			return 0;
		}
		const uint64 Hash = GetCityCellHash(Seed, Row, PairIndex, SideSign);
		static constexpr uint8 VillageCenterArchetypes[] = {5, 6, 7, 8, 11};
		static constexpr uint8 HomesteadArchetypes[] = {0, 1, 2, 3, 4, 6};
		static constexpr uint8 OuterArchetypes[] = {2, 4, 5, 8, 9, 10, 11};
		if ((Row % 17 == 0 && PairIndex % 11 < 3) || Row > LayoutRows * 3 / 4)
		{
			return OuterArchetypes[Hash % UE_ARRAY_COUNT(OuterArchetypes)];
		}
		if (PairIndex < LayoutColumns / 12)
		{
			return VillageCenterArchetypes[Hash % UE_ARRAY_COUNT(VillageCenterArchetypes)];
		}
		return HomesteadArchetypes[Hash % UE_ARRAY_COUNT(HomesteadArchetypes)];
	}

	bool BuildArchetypeAssignments(
		const int32 StructureCount,
		const TArray<UCityBuildingRecipe*>& Recipes,
		const int32 Seed,
		TArray<uint8>& OutAssignments,
		int64& OutEntityCount,
		FString& OutError)
	{
		OutAssignments.Reset();
		OutEntityCount = 0;
		OutError.Reset();
		if (StructureCount <= 0 || StructureCount > CompleteStructureCount || Recipes.Num() != 12)
		{
			OutError = TEXT("种子结构数量或配方目录无效。");
			return false;
		}
		TArray<int32, TInlineAllocator<16>> PieceCounts;
		PieceCounts.Reserve(Recipes.Num());
		for (const UCityBuildingRecipe* Recipe : Recipes)
		{
			if (!Recipe || Recipe->GetPieceEntityCount() <= 0)
			{
				OutError = TEXT("种子配方包含空项。");
				return false;
			}
			PieceCounts.Add(Recipe->GetPieceEntityCount());
		}

		OutAssignments.SetNumUninitialized(StructureCount);
		for (uint32 LayoutIndex = 0; LayoutIndex < static_cast<uint32>(StructureCount); ++LayoutIndex)
		{
			const uint8 ArchetypeIndex = ResolveArchetypeIndex(LayoutIndex, Seed);
			OutAssignments[LayoutIndex] = ArchetypeIndex;
			OutEntityCount += PieceCounts[ArchetypeIndex];
		}

		// 百万测试世界把 Entity 总数视为格式契约。Hash 先保留自然分布，再以升序
		// LayoutIndex 做最小量的确定性配方替换，将不同 Seed 的统计归一到批准值。
		if (StructureCount != CompleteStructureCount || OutEntityCount == ExpectedRecipeBuildingEntityCount)
		{
			return true;
		}
		int64 Remaining = FMath::Abs(OutEntityCount - ExpectedRecipeBuildingEntityCount);
		const bool bReduce = OutEntityCount > ExpectedRecipeBuildingEntityCount;
		for (int32 StructureIndex = 0; StructureIndex < OutAssignments.Num() && Remaining > 0; ++StructureIndex)
		{
			const uint8 CurrentArchetype = OutAssignments[StructureIndex];
			const int32 CurrentPieceCount = PieceCounts[CurrentArchetype];
			int32 BestDelta = 0;
			uint8 BestArchetype = CurrentArchetype;
			for (int32 CandidateIndex = 0; CandidateIndex < PieceCounts.Num(); ++CandidateIndex)
			{
				const int32 Delta = bReduce
					? CurrentPieceCount - PieceCounts[CandidateIndex]
					: PieceCounts[CandidateIndex] - CurrentPieceCount;
				if (Delta > BestDelta && Delta <= Remaining)
				{
					BestDelta = Delta;
					BestArchetype = static_cast<uint8>(CandidateIndex);
				}
			}
			if (BestDelta > 0)
			{
				OutAssignments[StructureIndex] = BestArchetype;
				Remaining -= BestDelta;
				OutEntityCount += bReduce ? -BestDelta : BestDelta;
			}
		}
		if (Remaining != 0 || OutEntityCount != ExpectedRecipeBuildingEntityCount)
		{
			OutAssignments.Reset();
			OutEntityCount = 0;
			OutError = TEXT("无法把百万结构配方分布归一到批准的 Building Entity 数。");
			return false;
		}
		return true;
	}

		FTransform ResolveStructureTransform(const uint32 LayoutIndex, const int32 Seed)
	{
		int32 Row = 0;
		int32 PairIndex = 0;
		int32 SideSign = 0;
		if (!TryResolveLayoutAddress(LayoutIndex, Row, PairIndex, SideSign))
		{
			return FTransform::Identity;
		}

			const uint64 PairHash = GetCityCellHash(Seed, Row, PairIndex, 1);
		const double ForwardJitter = HashToSignedRange(
			MixCityHash(PairHash ^ 0xa0761d6478bd642full), MaximumLotJitterCentimeters);
		const double SideJitter = HashToSignedRange(
			MixCityHash(PairHash ^ 0xe7037ed1a0b428dbull), MaximumLotJitterCentimeters);
		const double ForwardDistance = VillageGreenDepthCentimeters + HalfLotCentimeters
			+ Row * LotSpacingCentimeters + (Row / LotsPerLane) * LaneExtraCentimeters + ForwardJitter;
		const double SideDistance = ResolvePathCenterOffset(Seed, Row)
			+ SideSign * (MainPathHalfWidthCentimeters + HalfLotCentimeters + PairIndex * LotSpacingCentimeters
				+ (PairIndex / LotsPerLane) * LaneExtraCentimeters + SideJitter);
		const double YawJitter = HashToSignedRange(
			GetCityCellHash(Seed, Row, PairIndex, SideSign), MaximumYawJitterDegrees);
			return FTransform(
				FRotator(0.0, SideSign > 0 ? 90.0 + YawJitter : -90.0 + YawJitter, 0.0),
				FVector(ForwardDistance, SideDistance, 0.0));
		}

		FTransform ResolveSettlementTreeTransform(
			const uint32 LayoutIndex,
			const FVector2D& FootprintCentimeters,
			const int32 Seed)
		{
			if (LayoutIndex >= static_cast<uint32>(CompleteStructureCount)
				|| FootprintCentimeters.GetMin() <= UE_SMALL_NUMBER)
			{
				return FTransform::Identity;
			}
			const uint64 Hash = MixCityHash(
				static_cast<uint32>(Seed)
				^ (static_cast<uint64>(LayoutIndex) * 0xd6e8feb86659fd93ull)
				^ 0xa24baed4963ee407ull);
			const double SideSign = (Hash & 1ull) == 0 ? -1.0 : 1.0;
			const double SideOffset = SideSign * FMath::Clamp(
				FootprintCentimeters.Y * 0.35, 500.0, 1200.0);
			const double Yaw = static_cast<double>((Hash >> 8) & 0xffffffull)
				* 360.0 / static_cast<double>(0x1000000ull);
			const double ScaleUnit = static_cast<double>((Hash >> 32) & 0xffffffull)
				/ static_cast<double>(0xffffffull);
			const double UniformScale = FMath::Lerp(
				SettlementTreeMinimumUniformScale,
				SettlementTreeMaximumUniformScale,
				ScaleUnit);
			const FTransform LocalTreeTransform(
				FRotator(0.0, Yaw, 0.0),
				FVector(-(FootprintCentimeters.X * 0.5 + 350.0), SideOffset, 0.0),
				FVector(UniformScale));
			return LocalTreeTransform * ResolveStructureTransform(LayoutIndex, Seed);
		}

	FWorldEntityId ResolveSettlementTreeEntityId(
			const uint32 StructureIndex,
			const int64 BuildingEntityCount)
		{
			if (StructureIndex >= static_cast<uint32>(CompleteStructureCount)
				|| BuildingEntityCount <= 0)
			{
				return {};
			}
		return FWorldEntityId(static_cast<uint64>(BuildingEntityCount)
			+ static_cast<uint64>(StructureIndex) + 1ull);
	}

	FWorldEntityId ResolveSettlementMountedTorchEntityId(
		const uint32 StructureIndex,
		const uint8 FixtureIndex,
		const int64 RecipeBuildingEntityCount)
	{
		if (StructureIndex >= static_cast<uint32>(CompleteStructureCount)
			|| FixtureIndex >= MountedTorchFixturesPerStructure
			|| RecipeBuildingEntityCount <= 0)
		{
			return {};
		}
		return FWorldEntityId(static_cast<uint64>(RecipeBuildingEntityCount)
			+ static_cast<uint64>(StructureIndex) * MountedTorchFixturesPerStructure
			+ static_cast<uint64>(FixtureIndex) + 1ull);
	}

		bool TryParseTreeMode(const FString& Value, ESettlementTreeMode& OutMode)
		{
			if (Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				OutMode = ESettlementTreeMode::None;
				return true;
			}
			if (Value.Equals(TEXT("OnePerStructure"), ESearchCase::IgnoreCase))
			{
				OutMode = ESettlementTreeMode::OnePerStructure;
				return true;
			}
			return false;
		}

		const TCHAR* LexToString(const ESettlementTreeMode Mode)
		{
			return Mode == ESettlementTreeMode::OnePerStructure
				? TEXT("OnePerStructure")
				: TEXT("None");
		}

	int64 CountRecipeBuildingEntities(
		const int32 StructureCount,
		const TArray<UCityBuildingRecipe*>& Recipes,
		TArray<int64>* OutStructureCountByArchetype,
		const int32 Seed)
	{
		TArray<uint8> Assignments;
		int64 EntityCount = 0;
		FString Error;
		if (!BuildArchetypeAssignments(StructureCount, Recipes, Seed, Assignments, EntityCount, Error))
		{
			return 0;
		}
		if (OutStructureCountByArchetype)
		{
			OutStructureCountByArchetype->Init(0, Recipes.Num());
		}
		for (const uint8 ArchetypeIndex : Assignments)
		{
			if (OutStructureCountByArchetype)
			{
				++(*OutStructureCountByArchetype)[ArchetypeIndex];
			}
		}
		return EntityCount;
	}

		FGuid MakeWorldId(
			const int32 Seed,
			const int32 StructureCount,
			const ESettlementTreeMode TreeMode)
		{
		const uint64 TreeLayoutIdentity = TreeMode == ESettlementTreeMode::OnePerStructure
			? static_cast<uint64>(SettlementTreeLayoutRevision) << 48
			: 0ull;
		const uint64 BuildingLayoutIdentity =
			static_cast<uint64>(SettlementBuildingLayoutRevision) << 56;
		const uint64 LayoutIdentity =
			(static_cast<uint64>(static_cast<uint32>(StructureCount)) << 8)
			| static_cast<uint8>(TreeMode)
			| TreeLayoutIdentity
			| BuildingLayoutIdentity;
			const uint64 First = MixCityHash(
				static_cast<uint32>(Seed) ^ 0x6a09e667f3bcc909ull ^ LayoutIdentity);
			const uint64 Second = MixCityHash(
				static_cast<uint32>(Seed) ^ 0xbb67ae8584caa73bull
				^ (LayoutIdentity * 0x9e3779b97f4a7c15ull));
			return FGuid(
				static_cast<uint32>(First >> 32), static_cast<uint32>(First),
				static_cast<uint32>(Second >> 32), static_cast<uint32>(Second));
	}
}
