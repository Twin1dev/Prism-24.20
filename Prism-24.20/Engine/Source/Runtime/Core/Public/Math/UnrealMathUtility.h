#pragma once

/*=============================================================================
	UnrealMathUtility.h: The slice of FMath the Replication Graph uses, plus the
	FVector/FBox helpers the generated SDK's plain-old-data math types do not
	carry with them.
=============================================================================*/

#include <cmath>
#include <limits>

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

#define MAX_int32  0x7fffffff
#define MAX_uint16 0xffff
#define MAX_uint32 0xffffffffu
#define MAX_FLT    3.402823466e+38F

/** Written out rather than taken from std::numeric_limits, whose max() collides with the Windows max macro. */
inline constexpr double MAX_dbl = 1.7976931348623158e+308;

struct FMath
{
	template<typename T> static constexpr T Min(const T A, const T B) { return A < B ? A : B; }
	template<typename T> static constexpr T Max(const T A, const T B) { return A > B ? A : B; }
	template<typename T> static constexpr T Abs(const T A) { return A < static_cast<T>(0) ? -A : A; }
	template<typename T> static constexpr T Square(const T A) { return A * A; }

	template<typename T>
	static constexpr T Clamp(const T Value, const T MinValue, const T MaxValue)
	{
		return Value < MinValue ? MinValue : (Value > MaxValue ? MaxValue : Value);
	}

	static float Sqrt(float Value) { return std::sqrtf(Value); }
	static double Sqrt(double Value) { return std::sqrt(Value); }

	static float Loge(float Value) { return std::logf(Value); }

	static int32 TruncToInt(float Value) { return static_cast<int32>(Value); }
	static int32 TruncToInt(double Value) { return static_cast<int32>(Value); }

	static int32 FloorToInt(float Value) { return static_cast<int32>(std::floorf(Value)); }
	static int32 FloorToInt(double Value) { return static_cast<int32>(std::floor(Value)); }

	static int32 CeilToInt(float Value) { return static_cast<int32>(std::ceilf(Value)); }
	static int32 CeilToInt(double Value) { return static_cast<int32>(std::ceil(Value)); }

	static int32 RoundToInt(float Value) { return static_cast<int32>(std::floorf(Value + 0.5f)); }
	static int32 RoundToInt(double Value) { return static_cast<int32>(std::floor(Value + 0.5)); }

	static float FloatSelect(float Comparand, float ValueGEZero, float ValueLTZero) { return Comparand >= 0.f ? ValueGEZero : ValueLTZero; }

	static bool IsNearlyZero(float Value, float Tolerance = 1.e-4f) { return Abs(Value) <= Tolerance; }
	static bool IsNearlyEqual(float A, float B, float Tolerance = 1.e-4f) { return Abs(A - B) <= Tolerance; }

	static bool IsFinite(double Value) { return std::isfinite(Value); }

	/** Linear interpolation, matching FMath::Lerp. */
	template<typename T, typename U>
	static constexpr T Lerp(const T& A, const T& B, const U& Alpha) { return static_cast<T>(A + Alpha * (B - A)); }
};

/*-----------------------------------------------------------------------------
	Vector helpers
-----------------------------------------------------------------------------*/

/** FVector::SizeSquared. The SDK's FVector is plain data and carries no such member. */
inline double VectorSizeSquared(const FVector& V)
{
	return (V.X * V.X) + (V.Y * V.Y) + (V.Z * V.Z);
}

/** FVector::DistSquared. */
inline double VectorDistSquared(const FVector& A, const FVector& B)
{
	const double DX = A.X - B.X;
	const double DY = A.Y - B.Y;
	const double DZ = A.Z - B.Z;
	return (DX * DX) + (DY * DY) + (DZ * DZ);
}

/** FVector::DistSquaredXY - the spatialization node is 2D, so Z never enters the comparison. */
inline double VectorDistSquaredXY(const FVector& A, const FVector& B)
{
	const double DX = A.X - B.X;
	const double DY = A.Y - B.Y;
	return (DX * DX) + (DY * DY);
}

inline bool VectorContainsNaN(const FVector& V)
{
	return !FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y) || !FMath::IsFinite(V.Z);
}

/** Dot product of the normalized direction from A to B with Dir, used by the dynamic spatial frequency zones. */
inline double VectorDot(const FVector& A, const FVector& B)
{
	return (A.X * B.X) + (A.Y * B.Y) + (A.Z * B.Z);
}

inline FVector VectorSubtract(const FVector& A, const FVector& B)
{
	return FVector(A.X - B.X, A.Y - B.Y, A.Z - B.Z);
}

inline FVector VectorGetSafeNormal(const FVector& V)
{
	const double SizeSq = VectorSizeSquared(V);
	if (SizeSq <= 1.e-8)
	{
		return FVector(0.0, 0.0, 0.0);
	}

	const double Scale = 1.0 / FMath::Sqrt(SizeSq);
	return FVector(V.X * Scale, V.Y * Scale, V.Z * Scale);
}

PRISM_NAMESPACE_END
