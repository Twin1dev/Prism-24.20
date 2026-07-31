#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>

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