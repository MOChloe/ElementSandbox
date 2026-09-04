#include "WorldSeed/MillionBuildingRecipe.h"

#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogCityBuildingRecipe, Log, All);

namespace
{
constexpr double DefaultStoreyHeight = 300.0;

struct FCityArchetypeMetrics
{
		const TCHAR* DisplayName = TEXT("");
		FVector2D Footprint = FVector2D::ZeroVector;
		double Height = 0.0;
		FVector MountedTorchMountA = FVector::ZeroVector;
		FVector MountedTorchMountB = FVector::ZeroVector;
	};

enum class ECityPartCollision : uint8
{
	None,
	Simple
};

enum class ECityPartSurface : uint8
{
	Wall,
	Stone,
	Wood
};

FName GetCityPartSurfaceProfileId(const ECityPartSurface Surface)
{
	switch (Surface)
	{
	case ECityPartSurface::Wall:
		return TEXT("Surface.City.Wall");
	case ECityPartSurface::Stone:
		return TEXT("Surface.City.Stone");
	case ECityPartSurface::Wood:
		return TEXT("Surface.City.Wood");
	default:
		return NAME_None;
	}
}

struct FCityDoorOpening final
{
	double CenterY = 0.0;
	double Width = 150.0;
	double Height = 220.0;
};

class FCityPartBuilder final
{
public:
	explicit FCityPartBuilder(TArray<FCityBuildingPieceRecipe>& InPieces)
		: Pieces(InPieces)
	{
	}

	void AddBox(const FVector& Center, const FVector& Size, ECityPartSurface Surface,
		const FRotator& Rotation = FRotator::ZeroRotator,
		const ECityPartCollision Collision = ECityPartCollision::Simple)
	{
		check(Size.GetMin() > UE_SMALL_NUMBER);
		FCityBuildingPieceRecipe& Piece = Pieces.AddDefaulted_GetRef();
		Piece.Kind = ECityBuildingPieceKind::DecorativeBox;
		if (Collision == ECityPartCollision::Simple)
		{
			Piece.Kind = ECityBuildingPieceKind::SolidBox;
		}
		Piece.SurfaceProfileId = GetCityPartSurfaceProfileId(Surface);
		Piece.LocalTransform = FTransform(Rotation, Center, Size / 100.0);
	}

	void AddSphere(const FVector& Center, const FVector& Size, ECityPartSurface Surface,
		const ECityPartCollision Collision = ECityPartCollision::None)
	{
		check(Size.GetMin() > UE_SMALL_NUMBER);
		FCityBuildingPieceRecipe& Piece = Pieces.AddDefaulted_GetRef();
		Piece.Kind = ECityBuildingPieceKind::DecorativeSphere;
		if (Collision == ECityPartCollision::Simple)
		{
			Piece.Kind = ECityBuildingPieceKind::SolidSphere;
		}
		Piece.SurfaceProfileId = GetCityPartSurfaceProfileId(Surface);
		Piece.LocalTransform = FTransform(FQuat::Identity, Center, Size / 100.0);
	}

	void AddShell(const FVector& BaseCenter, const double SizeX, const double SizeY, const double Height,
		const ECityPartSurface Surface, const std::initializer_list<FCityDoorOpening> DoorOpenings = {},
		const double Thickness = 35.0)
	{
		check(SizeX > Thickness * 2.0 && SizeY > Thickness * 2.0 && Height > Thickness);
		const double WallZ = BaseCenter.Z + Height * 0.5;
		AddBox(FVector(BaseCenter.X, BaseCenter.Y - SizeY * 0.5, WallZ), FVector(SizeX, Thickness, Height), Surface);
		AddBox(FVector(BaseCenter.X, BaseCenter.Y + SizeY * 0.5, WallZ), FVector(SizeX, Thickness, Height), Surface);
		AddBox(FVector(BaseCenter.X + SizeX * 0.5, BaseCenter.Y, WallZ),
			FVector(Thickness, SizeY - Thickness * 2.0, Height), Surface);

		const double MinimumY = -SizeY * 0.5 + Thickness;
		const double MaximumY = SizeY * 0.5 - Thickness;
		double SegmentStartY = MinimumY;
		for (const FCityDoorOpening& Opening : DoorOpenings)
		{
			check(Opening.Width > 0.0 && Opening.Height > 0.0 && Opening.Height < Height);
			const double OpeningStartY = FMath::Clamp(Opening.CenterY - Opening.Width * 0.5, MinimumY, MaximumY);
			const double OpeningEndY = FMath::Clamp(Opening.CenterY + Opening.Width * 0.5, MinimumY, MaximumY);
			if (OpeningStartY > SegmentStartY + UE_SMALL_NUMBER)
			{
				const double SegmentWidth = OpeningStartY - SegmentStartY;
				AddBox(FVector(BaseCenter.X - SizeX * 0.5, BaseCenter.Y + SegmentStartY + SegmentWidth * 0.5, WallZ),
					FVector(Thickness, SegmentWidth, Height), Surface);
			}
			const double LintelHeight = Height - Opening.Height;
			AddBox(FVector(BaseCenter.X - SizeX * 0.5, BaseCenter.Y + (OpeningStartY + OpeningEndY) * 0.5,
					   BaseCenter.Z + Opening.Height + LintelHeight * 0.5),
				FVector(Thickness, OpeningEndY - OpeningStartY, LintelHeight), Surface);
			AddDoor(BaseCenter, SizeX, Opening.CenterY);
			SegmentStartY = OpeningEndY;
		}
		if (SegmentStartY < MaximumY - UE_SMALL_NUMBER)
		{
			const double SegmentWidth = MaximumY - SegmentStartY;
			AddBox(FVector(BaseCenter.X - SizeX * 0.5, BaseCenter.Y + SegmentStartY + SegmentWidth * 0.5, WallZ),
				FVector(Thickness, SegmentWidth, Height), Surface);
		}
	}

	void AddRusticWindows(const FVector& BuildingCenter, const double SizeX, const double SizeY, const int32 Storeys,
		const int32 Bays, const double FirstFloorBase = 0.0, const double StoreyHeight = DefaultStoreyHeight)
	{
		const int32 SafeBays = FMath::Clamp(Bays, 1, 5);
		const double BaySpan = SizeY / (SafeBays + 1);
		for (int32 Floor = 0; Floor < Storeys; ++Floor)
		{
			const double Z = BuildingCenter.Z + FirstFloorBase + Floor * StoreyHeight + StoreyHeight * 0.58;
			for (int32 Bay = 0; Bay < SafeBays; ++Bay)
			{
				const double Y = BuildingCenter.Y + (Bay - (SafeBays - 1) * 0.5) * BaySpan;
				const FVector WindowSize(18.0, FMath::Min(120.0, BaySpan * 0.42), 105.0);
				AddBox(FVector(BuildingCenter.X - SizeX * 0.5 - 10.0, Y, Z), WindowSize, ECityPartSurface::Wall,
					FRotator::ZeroRotator, ECityPartCollision::None);
				AddBox(FVector(BuildingCenter.X + SizeX * 0.5 + 10.0, Y, Z), WindowSize, ECityPartSurface::Wall,
					FRotator::ZeroRotator, ECityPartCollision::None);
			}
		}
	}

	void AddCornerPosts(const FVector& Center, const double SizeX, const double SizeY, const double Height,
		const double PostSize = 45.0)
	{
		for (const double X : { -SizeX * 0.5, SizeX * 0.5 })
		{
			for (const double Y : { -SizeY * 0.5, SizeY * 0.5 })
			{
				AddBox(FVector(Center.X + X, Center.Y + Y, Center.Z + Height * 0.5),
					FVector(PostSize, PostSize, Height), ECityPartSurface::Wood, FRotator::ZeroRotator,
					ECityPartCollision::None);
			}
		}
	}

	void AddTimberBands(const FVector& Center, const double SizeX, const double SizeY, const double Z)
	{
		AddBox(FVector(Center.X - SizeX * 0.5 - 10.0, Center.Y, Z), FVector(25.0, SizeY + 30.0, 35.0),
			ECityPartSurface::Wood, FRotator::ZeroRotator, ECityPartCollision::None);
		AddBox(FVector(Center.X + SizeX * 0.5 + 10.0, Center.Y, Z), FVector(25.0, SizeY + 30.0, 35.0),
			ECityPartSurface::Wood, FRotator::ZeroRotator, ECityPartCollision::None);
		AddBox(FVector(Center.X, Center.Y - SizeY * 0.5 - 10.0, Z), FVector(SizeX + 30.0, 25.0, 35.0),
			ECityPartSurface::Wood, FRotator::ZeroRotator, ECityPartCollision::None);
		AddBox(FVector(Center.X, Center.Y + SizeY * 0.5 + 10.0, Z), FVector(SizeX + 30.0, 25.0, 35.0),
			ECityPartSurface::Wood, FRotator::ZeroRotator, ECityPartCollision::None);
	}

	void AddCrossBrace(const FVector& Center, const double Span, const double Height, const double RollDegrees)
	{
		const double Length = FMath::Sqrt(Span * Span + Height * Height);
		AddBox(Center, FVector(24.0, Length, 28.0), ECityPartSurface::Wood, FRotator(0.0, 0.0, RollDegrees),
			ECityPartCollision::None);
	}

	void AddGableRoof(
		const FVector& Center, const double SizeX, const double SizeY, const double BaseZ, const double Rise)
	{
		const double HalfWidth = SizeX * 0.5 + 35.0;
		const double SlopeLength = FMath::Sqrt(HalfWidth * HalfWidth + Rise * Rise);
		const double Pitch = FMath::RadiansToDegrees(FMath::Atan2(Rise, HalfWidth));
		AddBox(FVector(Center.X - SizeX * 0.25, Center.Y, BaseZ + Rise * 0.5),
			FVector(SlopeLength, SizeY + 100.0, 35.0), ECityPartSurface::Wood, FRotator(Pitch, 0.0, 0.0));
		AddBox(FVector(Center.X + SizeX * 0.25, Center.Y, BaseZ + Rise * 0.5),
			FVector(SlopeLength, SizeY + 100.0, 35.0), ECityPartSurface::Wood, FRotator(-Pitch, 0.0, 0.0));
	}

	ECityPartSurface GetWall() const { return ECityPartSurface::Wall; }
	ECityPartSurface GetStone() const { return ECityPartSurface::Stone; }
	ECityPartSurface GetWood() const { return ECityPartSurface::Wood; }

private:
	void AddDoor(const FVector& BuildingBaseCenter, const double SizeX, const double DoorCenterY)
	{
			FCityBuildingPieceRecipe& Piece = Pieces.AddDefaulted_GetRef();
			Piece.Kind = ECityBuildingPieceKind::Door;
			Piece.SurfaceProfileId = GetCityPartSurfaceProfileId(ECityPartSurface::Wood);
			Piece.LocalTransform = FTransform(FRotator::ZeroRotator,
			FVector(
				BuildingBaseCenter.X - SizeX * 0.5 - 8.0, BuildingBaseCenter.Y + DoorCenterY, BuildingBaseCenter.Z));
	}

	TArray<FCityBuildingPieceRecipe>& Pieces;
};

FCityArchetypeMetrics BuildTimberCottage(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 900.0;
	constexpr double SizeY = 700.0;
	constexpr double WallHeight = 450.0;
	Builder.AddShell(FVector::ZeroVector, SizeX, SizeY, WallHeight, Builder.GetWall(), { { 0.0, 140.0, 215.0 } });
	Builder.AddBox(FVector(0.0, 0.0, 20.0), FVector(SizeX + 100.0, SizeY + 100.0, 40.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector::ZeroVector, SizeX, SizeY, WallHeight);
	Builder.AddTimberBands(FVector::ZeroVector, SizeX, SizeY, 220.0);
	Builder.AddRusticWindows(FVector::ZeroVector, SizeX, SizeY, 1, 2);
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, WallHeight, 240.0);
	Builder.AddBox(FVector(210.0, 170.0, 620.0), FVector(90.0, 90.0, 300.0), Builder.GetStone());
	return { TEXT("木梁小屋"), FVector2D(1100.0, 900.0), 760.0,
		FVector(-467.5, -180.0, 170.0), FVector(-467.5, 180.0, 170.0) };
}

FCityArchetypeMetrics BuildStoneCottage(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1100.0;
	constexpr double SizeY = 850.0;
	constexpr double WallHeight = 520.0;
	Builder.AddShell(FVector::ZeroVector, SizeX, SizeY, WallHeight, Builder.GetStone(), { { 0.0, 150.0, 220.0 } });
	Builder.AddBox(FVector(0.0, 0.0, 35.0), FVector(SizeX + 160.0, SizeY + 160.0, 70.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector::ZeroVector, SizeX + 30.0, SizeY + 30.0, 400.0, 70.0);
	Builder.AddRusticWindows(FVector::ZeroVector, SizeX, SizeY, 1, 2, 20.0);
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, WallHeight, 260.0);
	Builder.AddBox(FVector(250.0, 210.0, 675.0), FVector(120.0, 120.0, 410.0), Builder.GetStone());
	Builder.AddBox(FVector(-SizeX * 0.5 - 60.0, 0.0, 145.0), FVector(120.0, 420.0, 60.0), Builder.GetStone());
	return { TEXT("石砌小屋"), FVector2D(1300.0, 1050.0), 830.0,
		FVector(-567.5, -220.0, 190.0), FVector(-567.5, 220.0, 190.0) };
}

FCityArchetypeMetrics BuildFarmhouse(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1500.0;
	constexpr double SizeY = 1000.0;
	constexpr double WallHeight = 600.0;
	Builder.AddShell(FVector(100.0, 0.0, 0.0), SizeX, SizeY, WallHeight, Builder.GetWall(), { { 0.0, 160.0, 230.0 } });
	Builder.AddShell(FVector(-500.0, -650.0, 0.0), 700.0, 500.0, 460.0, Builder.GetWall());
	Builder.AddBox(FVector(100.0, 0.0, 20.0), FVector(SizeX + 130.0, SizeY + 130.0, 40.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector(100.0, 0.0, 0.0), SizeX, SizeY, WallHeight);
	Builder.AddTimberBands(FVector(100.0, 0.0, 0.0), SizeX, SizeY, 300.0);
	Builder.AddRusticWindows(FVector(100.0, 0.0, 0.0), SizeX, SizeY, 2, 2);
	Builder.AddGableRoof(FVector(100.0, 0.0, 0.0), SizeX, SizeY, WallHeight, 300.0);
	Builder.AddGableRoof(FVector(-500.0, -650.0, 0.0), 700.0, 500.0, 460.0, 170.0);
	Builder.AddBox(FVector(-SizeX * 0.5 - 120.0, 0.0, 260.0), FVector(230.0, 750.0, 30.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 210.0, -310.0, 125.0), FVector(45.0, 45.0, 250.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 210.0, 310.0, 125.0), FVector(45.0, 45.0, 250.0), Builder.GetWood());
	return { TEXT("农舍"), FVector2D(2000.0, 1700.0), 950.0,
		FVector(-667.5, -240.0, 210.0), FVector(-667.5, 240.0, 210.0) };
}

FCityArchetypeMetrics BuildLonghouse(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1000.0;
	constexpr double SizeY = 2500.0;
	constexpr double WallHeight = 650.0;
	Builder.AddShell(FVector::ZeroVector, SizeX, SizeY, WallHeight, Builder.GetWall(),
		{ { -700.0, 150.0, 230.0 }, { 700.0, 150.0, 230.0 } });
	Builder.AddBox(FVector(0.0, 0.0, 22.0), FVector(SizeX + 120.0, SizeY + 120.0, 44.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector::ZeroVector, SizeX, SizeY, WallHeight, 55.0);
	for (const double Y : { -800.0, 0.0, 800.0 })
	{
		Builder.AddBox(
			FVector(-SizeX * 0.5 - 12.0, Y, WallHeight * 0.5), FVector(45.0, 45.0, WallHeight), Builder.GetWood());
		Builder.AddBox(
			FVector(SizeX * 0.5 + 12.0, Y, WallHeight * 0.5), FVector(45.0, 45.0, WallHeight), Builder.GetWood());
	}
	Builder.AddTimberBands(FVector::ZeroVector, SizeX, SizeY, 320.0);
	Builder.AddRusticWindows(FVector::ZeroVector, SizeX, SizeY, 2, 4);
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, WallHeight, 330.0);
	return { TEXT("长屋"), FVector2D(1250.0, 2750.0), 1030.0,
		FVector(-517.5, -400.0, 210.0), FVector(-517.5, 400.0, 210.0) };
}

FCityArchetypeMetrics BuildBarn(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1700.0;
	constexpr double SizeY = 2200.0;
	constexpr double WallHeight = 700.0;
	Builder.AddShell(FVector::ZeroVector, SizeX, SizeY, WallHeight, Builder.GetWood());
	Builder.AddBox(FVector(0.0, 0.0, 24.0), FVector(SizeX + 140.0, SizeY + 140.0, 48.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector::ZeroVector, SizeX, SizeY, WallHeight, 65.0);
	for (const double Y : { -800.0, -400.0, 0.0, 400.0, 800.0 })
	{
		Builder.AddBox(FVector(-SizeX * 0.5 - 12.0, Y, 350.0), FVector(40.0, 35.0, 700.0), Builder.GetWood());
	}
	Builder.AddBox(FVector(-SizeX * 0.5 - 14.0, -330.0, 245.0), FVector(30.0, 620.0, 470.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 14.0, 330.0, 245.0), FVector(30.0, 620.0, 470.0), Builder.GetWood());
	Builder.AddCrossBrace(FVector(-SizeX * 0.5 - 28.0, -650.0, 350.0), 600.0, 500.0, 40.0);
	Builder.AddCrossBrace(FVector(-SizeX * 0.5 - 28.0, 650.0, 350.0), 600.0, 500.0, -40.0);
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, WallHeight, 420.0);
	return { TEXT("谷仓"), FVector2D(1950.0, 2450.0), 1170.0,
		FVector(-867.5, -400.0, 250.0), FVector(-867.5, 400.0, 250.0) };
}

FCityArchetypeMetrics BuildSmithy(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1200.0;
	constexpr double SizeY = 950.0;
	constexpr double WallHeight = 500.0;
	Builder.AddShell(
		FVector(150.0, 0.0, 0.0), SizeX, SizeY, WallHeight, Builder.GetStone(), { { -220.0, 150.0, 220.0 } });
	Builder.AddBox(FVector(150.0, 0.0, 20.0), FVector(SizeX + 120.0, SizeY + 120.0, 40.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector(150.0, 0.0, 0.0), SizeX, SizeY, WallHeight);
	Builder.AddRusticWindows(FVector(150.0, 0.0, 0.0), SizeX, SizeY, 1, 2);
	Builder.AddGableRoof(FVector(150.0, 0.0, 0.0), SizeX, SizeY, WallHeight, 260.0);
	Builder.AddBox(FVector(-SizeX * 0.5 - 180.0, 0.0, 270.0), FVector(360.0, 820.0, 35.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 320.0, -330.0, 135.0), FVector(50.0, 50.0, 270.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 320.0, 330.0, 135.0), FVector(50.0, 50.0, 270.0), Builder.GetWood());
	Builder.AddBox(FVector(320.0, 230.0, 720.0), FVector(170.0, 170.0, 600.0), Builder.GetStone());
	Builder.AddBox(FVector(-750.0, 120.0, 90.0), FVector(260.0, 220.0, 180.0), Builder.GetStone());
	Builder.AddBox(FVector(-820.0, -180.0, 80.0), FVector(200.0, 120.0, 160.0), Builder.GetWood());
	return { TEXT("铁匠铺"), FVector2D(1950.0, 1250.0), 1050.0,
		FVector(-467.5, -370.0, 190.0), FVector(-467.5, 260.0, 190.0) };
}

FCityArchetypeMetrics BuildTavern(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1600.0;
	constexpr double SizeY = 1300.0;
	constexpr double WallHeight = 850.0;
	Builder.AddShell(FVector::ZeroVector, SizeX, SizeY, WallHeight, Builder.GetWall(), { { 0.0, 190.0, 245.0 } });
	Builder.AddBox(FVector(0.0, 0.0, 24.0), FVector(SizeX + 160.0, SizeY + 160.0, 48.0), Builder.GetStone());
	Builder.AddCornerPosts(FVector::ZeroVector, SizeX, SizeY, WallHeight, 60.0);
	Builder.AddTimberBands(FVector::ZeroVector, SizeX, SizeY, 300.0);
	Builder.AddTimberBands(FVector::ZeroVector, SizeX, SizeY, 590.0);
	Builder.AddRusticWindows(FVector::ZeroVector, SizeX, SizeY, 3, 3, 0.0, 270.0);
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, WallHeight, 360.0);
	Builder.AddBox(FVector(-SizeX * 0.5 - 150.0, 0.0, 300.0), FVector(300.0, 1000.0, 35.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 270.0, -410.0, 150.0), FVector(55.0, 55.0, 300.0), Builder.GetWood());
	Builder.AddBox(FVector(-SizeX * 0.5 - 270.0, 410.0, 150.0), FVector(55.0, 55.0, 300.0), Builder.GetWood());
	Builder.AddBox(
		FVector(-SizeX * 0.5 + 20.0, SizeY * 0.5 + 120.0, 520.0), FVector(45.0, 240.0, 250.0), Builder.GetWood());
	Builder.AddBox(FVector(300.0, 260.0, 1040.0), FVector(110.0, 110.0, 430.0), Builder.GetStone());
	return { TEXT("乡村酒馆"), FVector2D(2050.0, 1650.0), 1250.0,
		FVector(-817.5, -280.0, 240.0), FVector(-817.5, 280.0, 240.0) };
}

FCityArchetypeMetrics BuildMarketHall(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 2200.0;
	constexpr double SizeY = 1700.0;
	constexpr double RoofBase = 620.0;
	Builder.AddBox(FVector(0.0, 0.0, 20.0), FVector(SizeX, SizeY, 40.0), Builder.GetStone());
	for (const double X : { -900.0, -300.0, 300.0, 900.0 })
	{
		for (const double Y : { -680.0, 680.0 })
		{
			Builder.AddBox(FVector(X, Y, RoofBase * 0.5), FVector(65.0, 65.0, RoofBase), Builder.GetWood());
		}
	}
	Builder.AddBox(FVector(0.0, -680.0, RoofBase - 40.0), FVector(SizeX, 70.0, 100.0), Builder.GetWood());
	Builder.AddBox(FVector(0.0, 680.0, RoofBase - 40.0), FVector(SizeX, 70.0, 100.0), Builder.GetWood());
	Builder.AddGableRoof(FVector::ZeroVector, SizeX, SizeY, RoofBase, 360.0);
	for (const double X : { -650.0, 0.0, 650.0 })
	{
		Builder.AddBox(FVector(X, -360.0, 105.0), FVector(380.0, 220.0, 210.0), Builder.GetWood());
		Builder.AddBox(FVector(X, 360.0, 105.0), FVector(380.0, 220.0, 210.0), Builder.GetWood());
	}
	return { TEXT("露天集市"), FVector2D(2400.0, 1900.0), 1030.0,
		FVector(-932.5, -680.0, 210.0), FVector(-932.5, 680.0, 210.0) };
}

FCityArchetypeMetrics BuildWatchtower(FCityPartBuilder& Builder)
{
	constexpr double TowerSize = 850.0;
	constexpr double PlatformZ = 1450.0;
	for (const double X : { -330.0, 330.0 })
	{
		for (const double Y : { -330.0, 330.0 })
		{
			Builder.AddBox(FVector(X, Y, PlatformZ * 0.5), FVector(85.0, 85.0, PlatformZ), Builder.GetWood());
		}
	}
	Builder.AddBox(FVector(0.0, 0.0, PlatformZ), FVector(1150.0, 1150.0, 90.0), Builder.GetWood());
	Builder.AddShell(FVector(0.0, 0.0, PlatformZ + 20.0), TowerSize, TowerSize, 520.0, Builder.GetWall());
	Builder.AddCornerPosts(FVector(0.0, 0.0, PlatformZ), TowerSize, TowerSize, 520.0, 55.0);
	for (const double Z : { 350.0, 750.0, 1150.0 })
	{
		Builder.AddBox(FVector(-340.0, 0.0, Z), FVector(50.0, 760.0, 45.0), Builder.GetWood());
		Builder.AddBox(FVector(340.0, 0.0, Z), FVector(50.0, 760.0, 45.0), Builder.GetWood());
	}
	Builder.AddRusticWindows(FVector(0.0, 0.0, PlatformZ), TowerSize, TowerSize, 1, 2, 30.0, 420.0);
	Builder.AddGableRoof(FVector(0.0, 0.0, 0.0), TowerSize, TowerSize, PlatformZ + 540.0, 300.0);
	return { TEXT("木制瞭望塔"), FVector2D(1300.0, 1300.0), 2310.0,
		FVector(-442.5, -220.0, 1610.0), FVector(-442.5, 220.0, 1610.0) };
}

FCityArchetypeMetrics BuildPalisadeGate(FCityPartBuilder& Builder)
{
	constexpr double TowerX = 720.0;
	constexpr double TowerY = 700.0;
	constexpr double TowerHeight = 1250.0;
	for (const double Y : { -1050.0, 1050.0 })
	{
		Builder.AddShell(FVector(0.0, Y, 0.0), TowerX, TowerY, TowerHeight, Builder.GetWood());
		Builder.AddCornerPosts(FVector(0.0, Y, 0.0), TowerX, TowerY, TowerHeight, 70.0);
		Builder.AddGableRoof(FVector(0.0, Y, 0.0), TowerX, TowerY, TowerHeight, 260.0);
	}
	for (const double Y : { -1450.0, -850.0, 850.0, 1450.0 })
	{
		Builder.AddBox(FVector(0.0, Y, 480.0), FVector(90.0, 90.0, 960.0), Builder.GetWood());
	}
	Builder.AddBox(FVector(0.0, 0.0, 1030.0), FVector(500.0, 1400.0, 110.0), Builder.GetWood());
	Builder.AddBox(FVector(-15.0, -330.0, 410.0), FVector(55.0, 620.0, 820.0), Builder.GetWood());
	Builder.AddBox(FVector(-15.0, 330.0, 410.0), FVector(55.0, 620.0, 820.0), Builder.GetWood());
	Builder.AddCrossBrace(FVector(-50.0, -330.0, 420.0), 560.0, 650.0, 48.0);
	Builder.AddCrossBrace(FVector(-50.0, 330.0, 420.0), 560.0, 650.0, -48.0);
	return { TEXT("木栅门楼"), FVector2D(1000.0, 3200.0), 1570.0,
		FVector(-377.5, -1050.0, 560.0), FVector(-377.5, 1050.0, 560.0) };
}

FCityArchetypeMetrics BuildWindmill(FCityPartBuilder& Builder)
{
	Builder.AddShell(FVector::ZeroVector, 1150.0, 1150.0, 600.0, Builder.GetStone(), { { 0.0, 150.0, 230.0 } });
	Builder.AddBox(FVector(0.0, 0.0, 600.0), FVector(1200.0, 1200.0, 50.0), Builder.GetWood());
	Builder.AddShell(FVector(0.0, 0.0, 600.0), 950.0, 950.0, 500.0, Builder.GetWall());
	Builder.AddBox(FVector(0.0, 0.0, 1100.0), FVector(1000.0, 1000.0, 50.0), Builder.GetWood());
	Builder.AddShell(FVector(0.0, 0.0, 1100.0), 760.0, 760.0, 350.0, Builder.GetWall());
	Builder.AddCornerPosts(FVector(0.0, 0.0, 600.0), 920.0, 920.0, 800.0, 50.0);
	Builder.AddRusticWindows(FVector(0.0, 0.0, 600.0), 950.0, 950.0, 2, 2, 0.0, 330.0);
	Builder.AddGableRoof(FVector::ZeroVector, 760.0, 760.0, 1450.0, 300.0);
	constexpr double HubX = -430.0;
	constexpr double HubZ = 1280.0;
	Builder.AddSphere(FVector(HubX, 0.0, HubZ), FVector(230.0), Builder.GetWood());
	Builder.AddBox(FVector(HubX, 0.0, HubZ), FVector(320.0, 70.0, 70.0), Builder.GetWood());
	for (int32 Blade = 0; Blade < 4; ++Blade)
	{
		const double Angle = Blade * 90.0 + 45.0;
		const double Radians = FMath::DegreesToRadians(Angle);
		Builder.AddBox(FVector(HubX - 20.0, FMath::Cos(Radians) * 420.0, HubZ + FMath::Sin(Radians) * 420.0),
			FVector(35.0, 170.0, 760.0), Builder.GetWood(), FRotator(0.0, 0.0, -Angle));
	}
	return { TEXT("风车磨坊"), FVector2D(1600.0, 1600.0), 1800.0,
		FVector(-592.5, -260.0, 210.0), FVector(-592.5, 260.0, 210.0) };
}

FCityArchetypeMetrics BuildVillageShrine(FCityPartBuilder& Builder)
{
	constexpr double SizeX = 1700.0;
	constexpr double SizeY = 1400.0;
	Builder.AddBox(FVector(0.0, 0.0, 35.0), FVector(SizeX, SizeY, 70.0), Builder.GetStone());
	Builder.AddBox(FVector(-150.0, 0.0, 105.0), FVector(1400.0, 1150.0, 70.0), Builder.GetStone());
	Builder.AddBox(FVector(-780.0, 0.0, 70.0), FVector(300.0, 700.0, 35.0), Builder.GetStone());
	for (const double X : { -500.0, 0.0, 500.0 })
	{
		for (const double Y : { -500.0, 500.0 })
		{
			Builder.AddBox(FVector(X, Y, 560.0), FVector(70.0, 70.0, 900.0), Builder.GetWood());
		}
	}
	Builder.AddBox(FVector(0.0, -500.0, 980.0), FVector(1500.0, 90.0, 100.0), Builder.GetWood());
	Builder.AddBox(FVector(0.0, 500.0, 980.0), FVector(1500.0, 90.0, 100.0), Builder.GetWood());
	Builder.AddGableRoof(FVector::ZeroVector, 1700.0, 1400.0, 1010.0, 380.0);
	Builder.AddGableRoof(FVector(-420.0, 0.0, 0.0), 650.0, 900.0, 720.0, 260.0);
	Builder.AddBox(FVector(260.0, 0.0, 240.0), FVector(260.0, 360.0, 300.0), Builder.GetStone());
	Builder.AddSphere(FVector(-420.0, 0.0, 780.0), FVector(180.0), Builder.GetStone());
	// 正立面的木质矮栏落在入口石台上，不保留无支撑的悬空横梁。
	Builder.AddBox(FVector(-SizeX * 0.5 - 40.0, 0.0, 105.0), FVector(35.0, 650.0, 210.0), Builder.GetWood());
	return { TEXT("村落神龛"), FVector2D(1900.0, 1650.0), 1430.0,
		FVector(-535.0, -500.0, 430.0), FVector(-535.0, 500.0, 430.0) };
}
}

TConstArrayView<ECityBuildingArchetype> GetDefaultCityBuildingArchetypes()
{
	static const ECityBuildingArchetype Archetypes[] = { ECityBuildingArchetype::TimberCottage,
		ECityBuildingArchetype::StoneCottage, ECityBuildingArchetype::Farmhouse, ECityBuildingArchetype::Longhouse,
		ECityBuildingArchetype::Barn, ECityBuildingArchetype::Smithy, ECityBuildingArchetype::Tavern,
		ECityBuildingArchetype::MarketHall, ECityBuildingArchetype::Watchtower, ECityBuildingArchetype::PalisadeGate,
		ECityBuildingArchetype::Windmill, ECityBuildingArchetype::VillageShrine };
	return Archetypes;
}

FName GetCityBuildingRecipeId(const ECityBuildingArchetype Archetype)
{
	switch (Archetype)
	{
	case ECityBuildingArchetype::TimberCottage:
		return TEXT("Settlement.Recipe.TimberCottage");
	case ECityBuildingArchetype::StoneCottage:
		return TEXT("Settlement.Recipe.StoneCottage");
	case ECityBuildingArchetype::Farmhouse:
		return TEXT("Settlement.Recipe.Farmhouse");
	case ECityBuildingArchetype::Longhouse:
		return TEXT("Settlement.Recipe.Longhouse");
	case ECityBuildingArchetype::Barn:
		return TEXT("Settlement.Recipe.Barn");
	case ECityBuildingArchetype::Smithy:
		return TEXT("Settlement.Recipe.Smithy");
	case ECityBuildingArchetype::Tavern:
		return TEXT("Settlement.Recipe.Tavern");
	case ECityBuildingArchetype::MarketHall:
		return TEXT("Settlement.Recipe.MarketHall");
	case ECityBuildingArchetype::Watchtower:
		return TEXT("Settlement.Recipe.Watchtower");
	case ECityBuildingArchetype::PalisadeGate:
		return TEXT("Settlement.Recipe.PalisadeGate");
	case ECityBuildingArchetype::Windmill:
		return TEXT("Settlement.Recipe.Windmill");
	case ECityBuildingArchetype::VillageShrine:
		return TEXT("Settlement.Recipe.VillageShrine");
	default:
		return NAME_None;
	}
}

namespace
{
	constexpr double AssemblyContactToleranceCentimeters = 2.0;

	FBox TransformRecipeBox(const FBox& LocalBounds, const FTransform& Transform)
	{
		FBox Result(ForceInit);
		for (const double X : {LocalBounds.Min.X, LocalBounds.Max.X})
		{
			for (const double Y : {LocalBounds.Min.Y, LocalBounds.Max.Y})
			{
				for (const double Z : {LocalBounds.Min.Z, LocalBounds.Max.Z})
				{
					Result += Transform.TransformPosition(FVector(X, Y, Z));
				}
			}
		}
		return Result;
	}

	FBox ResolveRecipePieceBounds(const FCityBuildingPieceRecipe& Piece)
	{
		if (Piece.IsDoor())
		{
			// Settlement.Door 的七个 Part 合并后约为 12 x 116 x 231 cm，原点在门底。
			return TransformRecipeBox(
				FBox(FVector(-6.0, -58.0, 0.0), FVector(6.0, 58.0, 231.0)),
				Piece.LocalTransform);
		}
		return TransformRecipeBox(
			FBox(FVector(-50.0), FVector(50.0)),
			Piece.LocalTransform);
	}

	bool RecipeBoundsTouch(const FBox& Left, const FBox& Right)
	{
		const double Tolerance = AssemblyContactToleranceCentimeters;
		return Left.Min.X <= Right.Max.X + Tolerance && Left.Max.X + Tolerance >= Right.Min.X
			&& Left.Min.Y <= Right.Max.Y + Tolerance && Left.Max.Y + Tolerance >= Right.Min.Y
			&& Left.Min.Z <= Right.Max.Z + Tolerance && Left.Max.Z + Tolerance >= Right.Min.Z;
	}

	bool RecipeBoundsContainMount(const FBox& Bounds, const FVector& Mount)
	{
		const double Tolerance = AssemblyContactToleranceCentimeters;
		return Mount.X >= Bounds.Min.X - Tolerance && Mount.X <= Bounds.Max.X + Tolerance
			&& Mount.Y >= Bounds.Min.Y - Tolerance && Mount.Y <= Bounds.Max.Y + Tolerance
			&& Mount.Z >= Bounds.Min.Z - Tolerance && Mount.Z <= Bounds.Max.Z + Tolerance;
	}
}

bool FCityBuildingPieceRecipe::IsValid() const
{
	switch (Kind)
	{
	case ECityBuildingPieceKind::SolidBox:
	case ECityBuildingPieceKind::DecorativeBox:
	case ECityBuildingPieceKind::SolidSphere:
	case ECityBuildingPieceKind::DecorativeSphere:
	case ECityBuildingPieceKind::Door:
		break;
	default:
		return false;
	}
	const FVector Scale = LocalTransform.GetScale3D();
	return !SurfaceProfileId.IsNone()
		&& !LocalTransform.ContainsNaN() && FMath::Abs(Scale.X) > UE_SMALL_NUMBER
		&& FMath::Abs(Scale.Y) > UE_SMALL_NUMBER && FMath::Abs(Scale.Z) > UE_SMALL_NUMBER;
}

bool UCityBuildingRecipe::ValidateAssemblyGeometry(FString* OutError) const
{
	if (OutError)
	{
		OutError->Reset();
	}
	const auto Fail = [OutError](const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	};
	if (Pieces.IsEmpty())
	{
		return Fail(TEXT("结构配方没有部件。"));
	}

	TArray<FBox> Bounds;
	Bounds.Reserve(Pieces.Num());
	TBitArray<> Grounded(false, Pieces.Num());
	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		if (!Pieces[PieceIndex].IsValid())
		{
			return Fail(FString::Printf(TEXT("部件 %d 配置无效。"), PieceIndex));
		}
		const FBox PieceBounds = ResolveRecipePieceBounds(Pieces[PieceIndex]);
		if (!PieceBounds.IsValid)
		{
			return Fail(FString::Printf(TEXT("部件 %d 无法生成有效 Bounds。"), PieceIndex));
		}
		Bounds.Add(PieceBounds);
		Grounded[PieceIndex] = PieceBounds.Min.Z <= AssemblyContactToleranceCentimeters
			&& PieceBounds.Max.Z >= -AssemblyContactToleranceCentimeters;
	}

	bool bExpanded = true;
	while (bExpanded)
	{
		bExpanded = false;
		for (int32 CandidateIndex = 0; CandidateIndex < Bounds.Num(); ++CandidateIndex)
		{
			if (Grounded[CandidateIndex])
			{
				continue;
			}
			for (int32 GroundedIndex = 0; GroundedIndex < Bounds.Num(); ++GroundedIndex)
			{
				if (Grounded[GroundedIndex]
					&& RecipeBoundsTouch(Bounds[CandidateIndex], Bounds[GroundedIndex]))
				{
					Grounded[CandidateIndex] = true;
					bExpanded = true;
					break;
				}
			}
		}
	}
	for (int32 PieceIndex = 0; PieceIndex < Grounded.Num(); ++PieceIndex)
	{
		if (!Grounded[PieceIndex])
		{
			return Fail(FString::Printf(
				TEXT("部件 %d 未接地，也未与任何已接地部件接触。"), PieceIndex));
		}
	}

	if (MountedTorchLocalTransforms.Num() != 2)
	{
		return Fail(TEXT("结构配方必须有两个挂墙火把安装点。"));
	}
	for (int32 MountIndex = 0; MountIndex < MountedTorchLocalTransforms.Num(); ++MountIndex)
	{
		const FTransform& MountTransform = MountedTorchLocalTransforms[MountIndex];
		if (MountTransform.ContainsNaN()
			|| !MountTransform.GetScale3D().Equals(FVector::OneVector))
		{
			return Fail(FString::Printf(TEXT("火把安装点 %d Transform 无效。"), MountIndex));
		}
		const FVector Mount = MountTransform.GetLocation();
		bool bAnchored = false;
		for (int32 PieceIndex = 0; PieceIndex < Bounds.Num(); ++PieceIndex)
		{
			if (Grounded[PieceIndex] && RecipeBoundsContainMount(Bounds[PieceIndex], Mount))
			{
				bAnchored = true;
				break;
			}
		}
		if (!bAnchored)
		{
			return Fail(FString::Printf(
				TEXT("火把安装点 %d 没有落在任何已接地构件表面。"), MountIndex));
		}
	}
	return true;
}

bool UCityBuildingRecipe::Initialize(const ECityBuildingArchetype InArchetype)
{
	const FName StableId = GetCityBuildingRecipeId(InArchetype);
	if (StableId.IsNone())
	{
		return false;
	}

	Pieces.Reset();
	MountedTorchLocalTransforms.Reset();
	FCityPartBuilder Builder(Pieces);

	FCityArchetypeMetrics Metrics;
	switch (InArchetype)
	{
	case ECityBuildingArchetype::TimberCottage:
		Metrics = BuildTimberCottage(Builder);
		break;
	case ECityBuildingArchetype::StoneCottage:
		Metrics = BuildStoneCottage(Builder);
		break;
	case ECityBuildingArchetype::Farmhouse:
		Metrics = BuildFarmhouse(Builder);
		break;
	case ECityBuildingArchetype::Longhouse:
		Metrics = BuildLonghouse(Builder);
		break;
	case ECityBuildingArchetype::Barn:
		Metrics = BuildBarn(Builder);
		break;
	case ECityBuildingArchetype::Smithy:
		Metrics = BuildSmithy(Builder);
		break;
	case ECityBuildingArchetype::Tavern:
		Metrics = BuildTavern(Builder);
		break;
	case ECityBuildingArchetype::MarketHall:
		Metrics = BuildMarketHall(Builder);
		break;
	case ECityBuildingArchetype::Watchtower:
		Metrics = BuildWatchtower(Builder);
		break;
	case ECityBuildingArchetype::PalisadeGate:
		Metrics = BuildPalisadeGate(Builder);
		break;
	case ECityBuildingArchetype::Windmill:
		Metrics = BuildWindmill(Builder);
		break;
	case ECityBuildingArchetype::VillageShrine:
		Metrics = BuildVillageShrine(Builder);
		break;
	default:
		return false;
	}

	if (Pieces.IsEmpty() || Metrics.Footprint.GetMin() <= UE_SMALL_NUMBER
		|| Metrics.Height <= UE_SMALL_NUMBER || Metrics.MountedTorchMountA.ContainsNaN()
		|| Metrics.MountedTorchMountB.ContainsNaN()
		|| Metrics.MountedTorchMountA.Equals(Metrics.MountedTorchMountB, UE_SMALL_NUMBER))
	{
		Pieces.Reset();
		return false;
	}
	for (const FCityBuildingPieceRecipe& Piece : Pieces)
	{
		if (!Piece.IsValid() || (Piece.IsDoor() && !Piece.LocalTransform.GetScale3D().Equals(FVector::OneVector)))
		{
			Pieces.Reset();
			return false;
		}
	}
	MountedTorchLocalTransforms.Emplace(FRotator::ZeroRotator, Metrics.MountedTorchMountA);
	MountedTorchLocalTransforms.Emplace(FRotator::ZeroRotator, Metrics.MountedTorchMountB);

	RecipeId = StableId;
	Archetype = InArchetype;
	DisplayName = FText::FromString(Metrics.DisplayName);
	NominalFootprintCentimeters = Metrics.Footprint;
	NominalHeightCentimeters = Metrics.Height;
	FString AssemblyError;
	if (!ValidateAssemblyGeometry(&AssemblyError))
	{
		UE_LOG(LogCityBuildingRecipe, Error,
			TEXT("结构配方 %s 的几何审计失败：%s"), *StableId.ToString(), *AssemblyError);
		RecipeId = NAME_None;
		Pieces.Reset();
		MountedTorchLocalTransforms.Reset();
		return false;
	}
	return true;
}

int32 UCityBuildingRecipe::GetDoorEntityCount() const
{
	int32 Count = 0;
	for (const FCityBuildingPieceRecipe& Piece : Pieces)
	{
		if (Piece.IsDoor())
		{
			++Count;
		}
	}
	return Count;
}
