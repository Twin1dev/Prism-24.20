#pragma once

/*
* Scalar math support for the SDK's FVector / FQuat / FRotator.
*
* Ported 1:1 from UnrealEngine:
*   Engine/Source/Runtime/Core/Public/Math/UnrealMathUtility.h
*   Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h
*   Engine/Source/Runtime/Core/Private/GenericPlatform/GenericPlatformMath.cpp
*
* Only the subset reached by the three math types is provided; the engine's
* vector-intrinsic paths are not, so every function here is the engine's own
* PLATFORM_ENABLE_VECTORINTRINSICS==0 reference implementation.
*
* Include order: this continues the FMath namespace UnrealContainers.hpp opens,
* so it must come after it (Basic.hpp already does that).
*/

#include <cmath>
#include <cstdint>
#include <type_traits>


// Copied verbatim from UnrealMathUtility.h so tolerances match the engine exactly.
#define UE_PI                               (3.1415926535897932f)
#define UE_SMALL_NUMBER                     (1.e-8f)
#define UE_KINDA_SMALL_NUMBER               (1.e-4f)
#define UE_BIG_NUMBER                       (3.4e+38f)
#define UE_EULERS_NUMBER                    (2.71828182845904523536f)
#define UE_GOLDEN_RATIO                     (1.6180339887498948482045868343656381f)
#define UE_FLOAT_NON_FRACTIONAL             (8388608.f)

#define UE_DOUBLE_PI                        (3.141592653589793238462643383279502884197169399)
#define UE_DOUBLE_SMALL_NUMBER              (1.e-8)
#define UE_DOUBLE_KINDA_SMALL_NUMBER        (1.e-4)
#define UE_DOUBLE_BIG_NUMBER                (3.4e+38)
#define UE_DOUBLE_EULERS_NUMBER             (2.7182818284590452353602874713526624977572)
#define UE_DOUBLE_GOLDEN_RATIO              (1.6180339887498948482045868343656381)
#define UE_DOUBLE_NON_FRACTIONAL            (4503599627370496.0)

#define UE_INV_PI                           (0.31830988618f)
#define UE_HALF_PI                          (1.57079632679f)
#define UE_TWO_PI                           (6.28318530717f)
#define UE_PI_SQUARED                       (9.86960440108f)

#define UE_DOUBLE_INV_PI                    (0.31830988618379067154)
#define UE_DOUBLE_HALF_PI                   (1.57079632679489661923)
#define UE_DOUBLE_TWO_PI                    (6.28318530717958647692)
#define UE_DOUBLE_PI_SQUARED                (9.86960440108935861883)

#define UE_DELTA                            (0.00001f)
#define UE_DOUBLE_DELTA                     (0.00001 )

#define UE_THRESH_POINT_ON_PLANE            (0.10f)
#define UE_THRESH_POINT_ON_SIDE             (0.20f)
#define UE_THRESH_POINTS_ARE_SAME           (0.00002f)
#define UE_THRESH_POINTS_ARE_NEAR           (0.015f)
#define UE_THRESH_NORMALS_ARE_SAME          (0.00002f)
#define UE_THRESH_UVS_ARE_SAME              (0.0009765625f)
#define UE_THRESH_VECTORS_ARE_NEAR          (0.0004f)
#define UE_THRESH_SPLIT_POLY_WITH_PLANE     (0.25f)
#define UE_THRESH_SPLIT_POLY_PRECISELY      (0.01f)
#define UE_THRESH_ZERO_NORM_SQUARED         (0.0001f)
#define UE_THRESH_NORMALS_ARE_PARALLEL      (0.999845f)
#define UE_THRESH_NORMALS_ARE_ORTHOGONAL    (0.017455f)
#define UE_THRESH_VECTOR_NORMALIZED         (0.01f)
#define UE_THRESH_QUAT_NORMALIZED           (0.01f)


#ifndef IMPORT_CPP_SDK_INTO_IDA
namespace UC
{
#endif // IMPORT_CPP_SDK_INTO_IDA

	/*
	* EForceInit, from Engine/Source/Runtime/Core/Public/CoreMiscDefines.h.
	*
	* Used by the ForceInit constructors on FVector / FQuat / FRotator. ENoInit is
	* not repeated here; UnrealContainers.hpp already provides it.
	*/
	enum EForceInit
	{
		ForceInit,
		ForceInitToZero
	};


	/*
	* The rest of FMath that FVector / FQuat / FRotator reach.
	*
	* Min, Max, Abs, Clamp and the bit helpers already live in the FMath namespace
	* UnrealContainers.hpp opens for the containers, so they are not repeated here.
	*
	* The float/double overload pairs are kept separate exactly as the engine has
	* them, because the engine deliberately gives some of them different bodies:
	* SinCos and FastAsin approximate in float and are exact in double.
	*/
	namespace FMath
	{
		/* --- Trigonometry -------------------------------------------------- */

		[[nodiscard]] inline float  Sin(float Value) { return sinf(Value); }
		[[nodiscard]] inline double Sin(double Value) { return sin(Value); }

		[[nodiscard]] inline float  Cos(float Value) { return cosf(Value); }
		[[nodiscard]] inline double Cos(double Value) { return cos(Value); }

		[[nodiscard]] inline float  Tan(float Value) { return tanf(Value); }
		[[nodiscard]] inline double Tan(double Value) { return tan(Value); }

		[[nodiscard]] inline float  Asin(float Value) { return asinf((Value < -1.f) ? -1.f : ((Value < 1.f) ? Value : 1.f)); }
		[[nodiscard]] inline double Asin(double Value) { return asin((Value < -1.0) ? -1.0 : ((Value < 1.0) ? Value : 1.0)); }

		[[nodiscard]] inline float  Acos(float Value) { return acosf((Value < -1.f) ? -1.f : ((Value < 1.f) ? Value : 1.f)); }
		[[nodiscard]] inline double Acos(double Value) { return acos((Value < -1.0) ? -1.0 : ((Value < 1.0) ? Value : 1.0)); }

		[[nodiscard]] inline float  Atan(float Value) { return atanf(Value); }
		[[nodiscard]] inline double Atan(double Value) { return atan(Value); }

		[[nodiscard]] inline float  Atan2(float Y, float X) { return atan2f(Y, X); }
		[[nodiscard]] inline double Atan2(double Y, double X) { return atan2(Y, X); }

		/*
		* Computes the sine and cosine of a scalar value.
		*
		* The float overload is the engine's 11-/10-degree minimax approximation;
		* the double overload is exact, as the engine notes "No approximations for
		* doubles".
		*/
		inline void SinCos(float* ScalarSin, float* ScalarCos, float Value)
		{
			// Map Value to y in [-pi,pi], x = 2*pi*quotient + remainder.
			float quotient = (UE_INV_PI * 0.5f) * Value;
			if (Value >= 0.0f)
			{
				quotient = (float)((int64_t)(quotient + 0.5f));
			}
			else
			{
				quotient = (float)((int64_t)(quotient - 0.5f));
			}
			float y = Value - UE_TWO_PI * quotient;

			// Map y to [-pi/2,pi/2] with sin(y) = sin(Value).
			float sign;
			if (y > UE_HALF_PI)
			{
				y = UE_PI - y;
				sign = -1.0f;
			}
			else if (y < -UE_HALF_PI)
			{
				y = -UE_PI - y;
				sign = -1.0f;
			}
			else
			{
				sign = +1.0f;
			}

			float y2 = y * y;

			// 11-degree minimax approximation
			*ScalarSin = (((((-2.3889859e-08f * y2 + 2.7525562e-06f) * y2 - 0.00019840874f) * y2 + 0.0083333310f) * y2 - 0.16666667f) * y2 + 1.0f) * y;

			// 10-degree minimax approximation
			float p = ((((-2.6051615e-07f * y2 + 2.4760495e-05f) * y2 - 0.0013888378f) * y2 + 0.041666638f) * y2 - 0.5f) * y2 + 1.0f;
			*ScalarCos = sign * p;
		}

		inline void SinCos(double* ScalarSin, double* ScalarCos, double Value)
		{
			// No approximations for doubles
			*ScalarSin = FMath::Sin(Value);
			*ScalarCos = FMath::Cos(Value);
		}

		/* --- Roots, powers, exponentials ------------------------------------ */

		[[nodiscard]] inline float  Sqrt(float Value) { return sqrtf(Value); }
		[[nodiscard]] inline double Sqrt(double Value) { return sqrt(Value); }

		[[nodiscard]] inline float  Pow(float A, float B) { return powf(A, B); }
		[[nodiscard]] inline double Pow(double A, double B) { return pow(A, B); }

		[[nodiscard]] inline float  Exp(float Value) { return expf(Value); }
		[[nodiscard]] inline double Exp(double Value) { return exp(Value); }

		[[nodiscard]] inline float  Loge(float Value) { return logf(Value); }
		[[nodiscard]] inline double Loge(double Value) { return log(Value); }

		[[nodiscard]] inline float  InvSqrt(float F) { return 1.0f / sqrtf(F); }
		[[nodiscard]] inline double InvSqrt(double F) { return 1.0 / sqrt(F); }

		[[nodiscard]] inline float  InvSqrtEst(float F) { return InvSqrt(F); }
		[[nodiscard]] inline double InvSqrtEst(double F) { return InvSqrt(F); }

		/*
		* Computes the ASin of a scalar value.
		*
		* @param Value  input angle
		* @return ASin of Value
		*/
		[[nodiscard]] inline float FastAsin(float Value)
		{
			// Note: FASTASIN_HALF_PI (1.5707963050f) rather than UE_HALF_PI; it is the
			// value that accompanied the minimax coefficients, and using it everywhere
			// inside this function is what makes FastAsin(0.0f) == 0.0f.
			const float FASTASIN_HALF_PI = 1.5707963050f;

			// Clamp input to [-1,1].
			const bool nonnegative = (Value >= 0.0f);
			const float x = FMath::Abs(Value);
			float omx = 1.0f - x;
			if (omx < 0.0f)
			{
				omx = 0.0f;
			}
			const float root = FMath::Sqrt(omx);
			// 7-degree minimax approximation
			float result = ((((((-0.0012624911f * x + 0.0066700901f) * x - 0.0170881256f) * x + 0.0308918810f) * x - 0.0501743046f) * x + 0.0889789874f) * x - 0.2145988016f) * x + FASTASIN_HALF_PI;
			result *= root;  // acos(|x|)
			// acos(x) = pi - acos(-x) when x < 0, asin(x) = pi/2 - acos(x)
			return (nonnegative ? FASTASIN_HALF_PI - result : result - FASTASIN_HALF_PI);
		}

		[[nodiscard]] inline double FastAsin(double Value)
		{
			// TODO: add fast approximation
			return FMath::Asin(Value);
		}

		/* --- Rounding and remainder ----------------------------------------- */

		[[nodiscard]] inline float  FloorToFloat(float F) { return floorf(F); }
		[[nodiscard]] inline double FloorToDouble(double F) { return floor(F); }

		[[nodiscard]] inline float  CeilToFloat(float F) { return ceilf(F); }
		[[nodiscard]] inline double CeilToDouble(double F) { return ceil(F); }

		[[nodiscard]] inline float  RoundToFloat(float F) { return FloorToFloat(F + 0.5f); }
		[[nodiscard]] inline double RoundToDouble(double F) { return FloorToDouble(F + 0.5); }

		[[nodiscard]] inline float  Floor(float F) { return FloorToFloat(F); }
		[[nodiscard]] inline double Floor(double F) { return FloorToDouble(F); }

		[[nodiscard]] inline float  Ceil(float F) { return CeilToFloat(F); }
		[[nodiscard]] inline double Ceil(double F) { return CeilToDouble(F); }

		[[nodiscard]] inline int32 RoundToInt(float F) { return (int32)FloorToFloat(F + 0.5f); }
		[[nodiscard]] inline int32 RoundToInt(double F) { return (int32)FloorToDouble(F + 0.5); }

		[[nodiscard]] inline int32 TruncToInt(float F) { return (int32)F; }
		[[nodiscard]] inline int32 TruncToInt(double F) { return (int32)F; }

		[[nodiscard]] inline int32 FloorToInt(float F) { return (int32)FloorToFloat(F); }
		[[nodiscard]] inline int32 FloorToInt(double F) { return (int32)FloorToDouble(F); }

		[[nodiscard]] inline int32 CeilToInt(float F) { return (int32)CeilToFloat(F); }
		[[nodiscard]] inline int32 CeilToInt(double F) { return (int32)CeilToDouble(F); }

		/*
		* Returns the floating-point remainder of X / Y.
		*
		* Warning: Always returns remainder toward 0, not toward the smaller multiple of Y.
		*          So for example Fmod(2.8f, 2) gives .8f as you would expect, however, Fmod(-2.8f, 2) gives -.8f, NOT 1.2f
		*
		* The engine ensure()s and returns 0 for a near-zero divisor; without the log
		* machinery available here the guard is kept and the report dropped.
		*/
		[[nodiscard]] inline float Fmod(float X, float Y)
		{
			const float AbsY = FMath::Abs(Y);
			if (AbsY <= UE_SMALL_NUMBER)
				return 0.0f;

			return fmodf(X, Y);
		}

		[[nodiscard]] inline double Fmod(double X, double Y)
		{
			const double AbsY = FMath::Abs(Y);
			if (AbsY <= UE_DOUBLE_SMALL_NUMBER)
				return 0.0;

			return fmod(X, Y);
		}

		/* --- Comparison and selection ---------------------------------------- */

		template<class T>
		[[nodiscard]] constexpr T Sign(const T A) { return (A > (T)0) ? (T)1 : ((A < (T)0) ? (T)-1 : (T)0); }

		template<class T>
		[[nodiscard]] constexpr T Min3(const T A, const T B, const T C) { return Min(Min(A, B), C); }

		template<class T>
		[[nodiscard]] constexpr T Max3(const T A, const T B, const T C) { return Max(Max(A, B), C); }

		template<class T>
		[[nodiscard]] constexpr T Square(const T A) { return A * A; }

		template<class T>
		[[nodiscard]] constexpr T Cube(const T A) { return A * A * A; }

		[[nodiscard]] constexpr float FloatSelect(float Comparand, float ValueGEZero, float ValueLTZero)
		{
			return Comparand >= 0.f ? ValueGEZero : ValueLTZero;
		}

		[[nodiscard]] constexpr double FloatSelect(double Comparand, double ValueGEZero, double ValueLTZero)
		{
			return Comparand >= 0.0 ? ValueGEZero : ValueLTZero;
		}

		[[nodiscard]] inline bool IsNearlyEqual(float A, float B, float ErrorTolerance = UE_SMALL_NUMBER)
		{
			return Abs<float>(A - B) <= ErrorTolerance;
		}

		[[nodiscard]] inline bool IsNearlyEqual(double A, double B, double ErrorTolerance = UE_DOUBLE_SMALL_NUMBER)
		{
			return Abs<double>(A - B) <= ErrorTolerance;
		}

		[[nodiscard]] inline bool IsNearlyZero(float Value, float ErrorTolerance = UE_SMALL_NUMBER)
		{
			return Abs<float>(Value) <= ErrorTolerance;
		}

		[[nodiscard]] inline bool IsNearlyZero(double Value, double ErrorTolerance = UE_DOUBLE_SMALL_NUMBER)
		{
			return Abs<double>(Value) <= ErrorTolerance;
		}

		[[nodiscard]] inline bool IsNaN(float A) { return std::isnan(A); }
		[[nodiscard]] inline bool IsNaN(double A) { return std::isnan(A); }

		[[nodiscard]] inline bool IsFinite(float A) { return std::isfinite(A); }
		[[nodiscard]] inline bool IsFinite(double A) { return std::isfinite(A); }

		/* --- Interpolation and snapping --------------------------------------- */

		template<class T, class U>
		[[nodiscard]] constexpr T Lerp(const T& A, const T& B, const U& Alpha)
		{
			return (T)(A + Alpha * (B - A));
		}

		/** Snaps a value to the nearest grid multiple */
		template<class T>
		[[nodiscard]] inline T GridSnap(T Location, T Grid)
		{
			return (Grid == T{}) ? Location : (Floor((Location + (Grid / (T)2)) / Grid) * Grid);
		}

		/** Utility to ensure angle is between +/- 180 degrees by unwinding. */
		template<class T>
		[[nodiscard]] constexpr T UnwindDegrees(T A)
		{
			while (A > (T)180)
			{
				A -= (T)360;
			}

			while (A < (T)-180)
			{
				A += (T)360;
			}

			return A;
		}

		/** Utility to ensure angle is between +/- PI radians by unwinding. */
		template<class T>
		[[nodiscard]] constexpr T UnwindRadians(T A)
		{
			while (A > (T)UE_DOUBLE_PI)
			{
				A -= (T)UE_DOUBLE_TWO_PI;
			}

			while (A < (T)-UE_DOUBLE_PI)
			{
				A += (T)UE_DOUBLE_TWO_PI;
			}

			return A;
		}

		/* --- Angle conversion --------------------------------------------------- */

		[[nodiscard]] constexpr float  RadiansToDegrees(float const& RadVal) { return RadVal * (180.f / UE_PI); }
		[[nodiscard]] constexpr double RadiansToDegrees(double const& RadVal) { return RadVal * (180.0 / UE_DOUBLE_PI); }

		[[nodiscard]] constexpr float  DegreesToRadians(float const& DegVal) { return DegVal * (UE_PI / 180.f); }
		[[nodiscard]] constexpr double DegreesToRadians(double const& DegVal) { return DegVal * (UE_DOUBLE_PI / 180.0); }
	}

#ifndef IMPORT_CPP_SDK_INTO_IDA
}
#endif // IMPORT_CPP_SDK_INTO_IDA
