#pragma once

/*=============================================================================
	Casts.h: Cast<> for both halves of the object model this module lives in.

	Objects that came out of the game are UObjects and are tested through their
	UClass, exactly as the engine tests them. The Replication Graph's own nodes
	are plain C++ objects (see the note at the top of ReplicationGraphTypes.h)
	and are tested through the C++ type system. One spelling covers both so that
	code copied from the engine does not have to care which it is holding.
=============================================================================*/

#include <type_traits>

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

template<typename To, typename From>
inline To* Cast(From* Src)
{
	if constexpr (std::is_base_of_v<SDK_NAMESPACE_NAME::UObject, std::remove_const_t<From>>)
	{
		if (Src == nullptr || !Src->IsA(To::StaticClass()))
		{
			return nullptr;
		}

		/*
		 * Reinterpreted rather than static_cast, because the two types are not always related
		 * by inheritance even though every object is: a Prism extension and the generated class
		 * it is a view onto are siblings under SDK::UObject, so casting Prism::AActor* to
		 * SDK::AFortPlayerPawn* is a sideways move that static_cast will not make.
		 *
		 * The addresses are the same either way. Every generated class is single inheritance
		 * from UObject with no virtual bases, and a Prism extension adds no data members - which
		 * is what UE_VERIFY_SDK_CLASS_EXTENSION asserts - so there is no subobject offset for a
		 * cast to adjust. The IsA above is what makes the reinterpretation safe.
		 */
		using ToPointer = std::conditional_t<std::is_const_v<From>, const To*, To*>;
		return const_cast<To*>(reinterpret_cast<ToPointer>(Src));
	}
	else
	{
		return dynamic_cast<To*>(Src);
	}
}

template<typename To, typename From>
inline To* CastChecked(From* Src)
{
	return Cast<To>(Src);
}

/** Walks up to the first class that is not a Blueprint generated class. */
inline UClass* GetParentNativeClass(UClass* Class)
{
	while (Class && Class->GetName().find("_C") == (Class->GetName().length() - 2) && Class->GetName().length() > 2)
	{
		Class = static_cast<UClass*>(Class->SuperStruct);
	}

	return Class;
}

inline std::string GetNameSafe(const UObject* Object) { return Object ? Object->GetName() : std::string("None"); }

PRISM_NAMESPACE_END
