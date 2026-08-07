#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>

#include <type_traits>

#include <Engine/Restricted/NotForLicensees/Source/ThirdParty/SoftwareDevelopmentKit/SDK.hpp>

using namespace SDK;

#include "Logging/LogMacros.h"

#define PRISM_NAMESPACE_NAME Prism
#define PRISM_NAMESPACE_START namespace PRISM_NAMESPACE_NAME {
#define PRISM_NAMESPACE_END }
#define PRISM_PROPERTY_OVERRIDE(Type, Name) \
    PRISM_NAMESPACE_NAME::Type& Name##_Get() \
	{ \
		return *reinterpret_cast<PRISM_NAMESPACE_NAME::Type*>(Name); \
	} \
    void Name##_Set(std::add_lvalue_reference_t<std::add_const_t<Type>> Value) { Name##_Get() = Value; } \
	__declspec(property(get = Name##_Get, put = Name##_Set)) PRISM_NAMESPACE_NAME::Type Name

/*-----------------------------------------------------------------------------
	Reaching the Prism extension of a generated type
-----------------------------------------------------------------------------*/

PRISM_NAMESPACE_START

/**
 * The Prism extension that shadows a generated type, e.g. TPrismTypeOf<SDK::AFortInventory>::Type
 * is Prism::AFortInventory. Specialised by UE_DECLARE_PRISM_TYPE; there is deliberately no default,
 * so asking for a type nothing has registered is a compile error rather than a silent no-op.
 */
template<typename T>
struct TPrismTypeOf
{
	static_assert(sizeof(T) == 0,
		"No Prism extension is registered for this type. Include the header that declares it, and "
		"check that header uses UE_DECLARE_PRISM_TYPE after the class.");

	using Type = T;
};

/** TPrismTypeOf<T>::Type, carrying T's constness across. */
template<typename T>
using TPrismTypeOfT = std::conditional_t<std::is_const_v<T>,
	const typename TPrismTypeOf<std::remove_cv_t<T>>::Type,
	typename TPrismTypeOf<std::remove_cv_t<T>>::Type>;

/**
 * The same object, seen through the Prism extension of its type - so a member the generated headers
 * type as SDK::AFortInventory* can be used for the calls Prism::AFortInventory adds:
 *
 *     ToPrism(FortPC->WorldInventory)->AddItem(StartingItem.Item, StartingItem.Count);
 *
 * Nothing moves and nothing is checked: an extension adds no data members (which is what
 * UE_VERIFY_SDK_CLASS_EXTENSION asserts), so both types are the same bytes and the pointer is
 * unchanged. Passing a pointer that is already the Prism type is fine and hands it straight back.
 */
template<typename FromType>
[[nodiscard]] inline TPrismTypeOfT<FromType>* ToPrism(FromType* Object)
{
	using ToType = TPrismTypeOfT<FromType>;

	if constexpr (std::is_base_of_v<FromType, ToType>)
	{
		static_assert(sizeof(ToType) == sizeof(FromType),
			"The Prism extension changed the layout of the generated type, so the two are not the same object.");

		return static_cast<ToType*>(Object);
	}
	else
	{
		// The extension is a view onto a type it does not derive from - the way UActorChannel
		// extends UChannel because the generated UActorChannel is 'final'. It names a prefix of
		// the object and reaches anything past that by offset, so it may be the smaller of the two.
		static_assert(sizeof(ToType) <= sizeof(FromType),
			"The Prism view is larger than the generated type it is a view onto.");

		return reinterpret_cast<ToType*>(Object);
	}
}

/** Reference form of ToPrism, for members that are held by value rather than by pointer. */
template<typename FromType, typename = std::enable_if_t<!std::is_pointer_v<FromType>>>
[[nodiscard]] inline TPrismTypeOfT<FromType>& ToPrism(FromType& Object)
{
	return *ToPrism(&Object);
}

PRISM_NAMESPACE_END

/**
 * Registers a class as the Prism extension of the generated class of the same name, which is what
 * lets ToPrism() find it. Place it after the class, next to UE_VERIFY_SDK_CLASS_EXTENSION:
 *
 *     class AFortInventory : public UE_EXTENDS_SDK_CLASS(AFortInventory)
 *     {
 *         ...
 *     };
 *     UE_VERIFY_SDK_CLASS_EXTENSION(AFortInventory);
 *     UE_DECLARE_PRISM_TYPE(AFortInventory);
 *
 * Namespace scope only, and inside PRISM_NAMESPACE_START, since it specialises a Prism template.
 */
#define UE_DECLARE_PRISM_TYPE(ClassName)                                                            \
	template<> struct TPrismTypeOf<SDK_NAMESPACE_NAME ::ClassName> { using Type = ClassName; };     \
	template<> struct TPrismTypeOf<ClassName> { using Type = ClassName; };                          \
	static_assert(true, "")

/**
 * Registers an extension that is a view onto a generated class it does not derive from, because
 * that generated class is 'final'. ToPrism() reinterprets rather than casts for these, so what can
 * be checked is only that the view does not reach past the object - a view names a prefix of the
 * generated class and reaches anything beyond it by offset, the way UActorChannel reaches Actor.
 */
#define UE_DECLARE_PRISM_VIEW_TYPE(ClassName)                                                       \
	static_assert(sizeof(ClassName) <= sizeof(SDK_NAMESPACE_NAME ::ClassName),                      \
		#ClassName " is a view onto the generated class, so it may not be larger than it.");        \
	UE_DECLARE_PRISM_TYPE(ClassName)