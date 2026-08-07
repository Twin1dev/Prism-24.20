#pragma once

/*=============================================================================
	Array.h: The engine's array types, as this module names them.

	These are not reimplementations. UnrealContainers.hpp ports the engine's own
	TArray - same layout, same growth policy, same allocator policies - onto the
	game's FMalloc, and this header simply gives those types the names the rest
	of the module uses.

	Why that matters here rather than a container of our own:

	  * A TArray built by this module and one built by the game are the same
	    bytes and the same heap, so one can be handed to the other without a
	    copy or a translation layer.

	  * The allocator policies are real. TArray<FNetViewer, TInlineAllocator<4>>
	    actually keeps its first four viewers inside the array object, which is
	    what the engine's declaration promises and what the Replication Graph
	    relies on to keep its per-connection gather allocation-free.

	  * Every element goes through the engine's construct / destruct / relocate
	    passes, so move-only element types (TUniquePtr) and trivially
	    relocatable ones behave exactly as they do in the engine.

	Only the two types the engine keeps in Core and the port does not carry -
	TStaticArray and the module's own INDEX_NONE spelling - are defined here.
=============================================================================*/

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

/*-----------------------------------------------------------------------------
	Names
-----------------------------------------------------------------------------*/

using UC::INDEX_NONE;

/** Shrinking policy for the removal functions. */
using EAllowShrinking = UC::EAllowShrinking;

/*-----------------------------------------------------------------------------
	Allocator policies
-----------------------------------------------------------------------------*/

using FDefaultAllocator = UC::FDefaultAllocator;

/** Keeps the first NumInlineElements elements inside the container itself. */
template<uint32 NumInlineElements, typename SecondaryAllocator = FDefaultAllocator>
using TInlineAllocator = UC::TInlineAllocator<NumInlineElements, SecondaryAllocator>;

/** Inline storage with no fallback: growing past NumInlineElements is a programming error. */
template<uint32 NumInlineElements>
using TFixedAllocator = UC::TFixedAllocator<NumInlineElements>;

/*-----------------------------------------------------------------------------
	TArray / TArrayView
-----------------------------------------------------------------------------*/

template<typename InElementType, typename InAllocator = FDefaultAllocator>
using TArray = UC::TArray<InElementType, InAllocator>;

/** Non-owning window onto a contiguous run of elements. */
template<typename InElementType>
using TArrayView = UC::TArrayView<InElementType>;

/*-----------------------------------------------------------------------------
	TStaticArray

	Lives in Core rather than in the container port, because it has no engine
	allocation behaviour to match - it is exactly its elements and nothing else.
-----------------------------------------------------------------------------*/

/** Fixed size array, used where the engine needs one element per enum value. */
template<typename InElementType, uint32 NumElements>
class TStaticArray
{
public:
	using ElementType = InElementType;

	ElementType& operator[](uint32 Index) { return Storage[Index]; }
	const ElementType& operator[](uint32 Index) const { return Storage[Index]; }

	constexpr int32 Num() const { return static_cast<int32>(NumElements); }

	ElementType* begin() { return &Storage[0]; }
	const ElementType* begin() const { return &Storage[0]; }
	ElementType* end() { return &Storage[0] + NumElements; }
	const ElementType* end() const { return &Storage[0] + NumElements; }

private:

	ElementType Storage[NumElements] = {};
};

PRISM_NAMESPACE_END
