#pragma once

/*=============================================================================
	Stack.h: The slice of FFrame a replacement exec function needs.

	A native UFunction is reached from script through a generated thunk that
	unpacks the parameter frame and then calls the C++ implementation. When that
	implementation has been compiled out of this binary - which is what happened
	to UFortKismetLibrary::PickLootDrops, see FortKismetLibrary.cpp - the only
	place left to stand is the thunk itself, and standing there means doing the
	unpacking by hand.

	Everything below was recovered from
	  C:\NewDumps\24.20\24.20\FortniteClient-Win64-Shipping_dump_SCY.exe.i64
	(++Fortnite+Release-24.20-CL-25156858), by reading
	UFortKismetLibrary::execPickLootDrops (0xA10BC68) and the two functions it
	dispatches through. Each entry records how it was found so it can be
	re-derived against another build rather than re-guessed.
=============================================================================*/

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

/**
 * A link in UFunction's property chain, which is all StepCompiledIn walks.
 *
 * The thunk reads the next link as `mov rax, [r8+20h]`, so Next is at 0x20 -
 * FField::Next, this build's properties being FField-based rather than UObjects.
 */
struct FFieldChainLink
{
	uint8 Pad_00[0x20];
	FFieldChainLink* Next;
};

/**
 * The script execution frame, as this build lays it out.
 *
 * Offsets, and where each one was read:
 *
 *     0x18 Object                      execPickLootDrops: `mov rdx, [rbx+18h]` feeding Step
 *     0x20 Code                        execPickLootDrops: `cmp [rbx+20h], rsi` guarding every
 *                                      step, and `add [rbx+20h], rax` as P_FINISH
 *     0x28 Locals                      StepExplicitProperty (0xF6A604): `mov rcx, [rcx+28h]`
 *                                      before adding the property's Offset_Internal
 *     0x38 MostRecentPropertyAddress   StepExplicitProperty: `mov [rdi+38h], rax`
 *     0x40 MostRecentPropertyContainer StepExplicitProperty: `mov [rdi+40h], rcx`, and
 *                                      `and qword ptr [rcx+40h], 0` on the out-parameter path
 *     0x80 OutParms                    StepExplicitProperty: `mov rax, [rcx+80h]`, the list it
 *                                      walks for a CPF_OutParm property
 *     0x88 PropertyChainForCompiledIn  execPickLootDrops: `mov r8, [rbx+88h]`
 *
 * Node and MostRecentProperty are not read by anything here; they are named because
 * the layout is contiguous and leaving them as padding would hide what the gaps are.
 */
class FFrame
{
public:
	/** FOutputDevice: one vtable pointer and two bools, padded to 0x10. */
	void* OutputDeviceVTable;                       // 0x00
	uint8 Pad_08[0x08];                             // 0x08

	class UFunction* Node;                          // 0x10
	class UObject* Object;                          // 0x18
	uint8* Code;                                    // 0x20
	uint8* Locals;                                  // 0x28

	void* MostRecentProperty;                       // 0x30
	uint8* MostRecentPropertyAddress;               // 0x38
	uint8* MostRecentPropertyContainer;             // 0x40

	uint8 Pad_48[0x38];                             // 0x48
	void* OutParms;                                 // 0x80
	FFieldChainLink* PropertyChainForCompiledIn;    // 0x88

public:
	/**
	 * void FFrame::Step(UObject* Context, void* const Result)
	 *
	 * 0xF6B17C. Ten instructions: read the opcode at Code, advance Code past it, and tail-jump
	 * into GNatives through it. That shape is Step and nothing else.
	 */
	void Step(class UObject* Context, void* const Result)
	{
		static void (*Function)(FFrame*, class UObject*, void* const) = decltype(Function)(InSDKUtils::GetImageBase() + 0x0F6B17C);
		Function(this, Context, Result);
	}

	/**
	 * void FFrame::StepExplicitProperty(void* const Result, FProperty* Property)
	 *
	 * 0xF6A604. Tests bit 8 of the property's flags - CPF_OutParm - and either walks OutParms for
	 * the matching record or points MostRecentPropertyAddress at Locals + Offset_Internal, then
	 * copies the value out through the property's vtable.
	 */
	void StepExplicitProperty(void* const Result, FFieldChainLink* Property)
	{
		static void (*Function)(FFrame*, void* const, FFieldChainLink*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x0F6A604);
		Function(this, Result, Property);
	}

	/**
	 * Reads one parameter.
	 *
	 * Called from Blueprint bytecode there is code to step through; called from ProcessEvent with
	 * a prepared parameter struct there is not, and the frame carries the property chain instead.
	 * The engine's P_GET_* macros pick between the two exactly like this.
	 */
	void StepCompiledIn(void* const Result)
	{
		if (Code)
		{
			Step(Object, Result);
		}
		else
		{
			FFieldChainLink* Property = PropertyChainForCompiledIn;
			PropertyChainForCompiledIn = Property->Next;
			StepExplicitProperty(Result, Property);
		}
	}

	/**
	 * Reads one by-reference parameter, and hands back the caller's storage rather than a copy.
	 *
	 * TemporaryBuffer is only used when the step did not resolve to a real property address, which
	 * is what the engine's P_GET_TARRAY_REF does - the out value is written straight into the
	 * caller's array when there is one, so an out parameter actually comes back out.
	 */
	template<typename ValueType>
	ValueType& StepCompiledInRef(void* const TemporaryBuffer)
	{
		MostRecentPropertyAddress = nullptr;
		MostRecentPropertyContainer = nullptr;

		StepCompiledIn(TemporaryBuffer);

		return MostRecentPropertyAddress != nullptr
			? *reinterpret_cast<ValueType*>(MostRecentPropertyAddress)
			: *reinterpret_cast<ValueType*>(TemporaryBuffer);
	}

	/** P_FINISH: steps past the EX_EndFunctionParms the parameter list ends with. */
	void Finish()
	{
		if (Code)
			++Code;
	}
};

/* The padding above is arithmetic, so it is worth having the compiler check it against the
 * offsets the disassembly gave. Getting one of these wrong hands Step the wrong pointer. */
static_assert(offsetof(FFrame, Object) == 0x18, "FFrame::Object moved");
static_assert(offsetof(FFrame, Code) == 0x20, "FFrame::Code moved");
static_assert(offsetof(FFrame, Locals) == 0x28, "FFrame::Locals moved");
static_assert(offsetof(FFrame, MostRecentPropertyAddress) == 0x38, "FFrame::MostRecentPropertyAddress moved");
static_assert(offsetof(FFrame, MostRecentPropertyContainer) == 0x40, "FFrame::MostRecentPropertyContainer moved");
static_assert(offsetof(FFrame, OutParms) == 0x80, "FFrame::OutParms moved");
static_assert(offsetof(FFrame, PropertyChainForCompiledIn) == 0x88, "FFrame::PropertyChainForCompiledIn moved");
static_assert(offsetof(FFieldChainLink, Next) == 0x20, "FField::Next moved");

PRISM_NAMESPACE_END
