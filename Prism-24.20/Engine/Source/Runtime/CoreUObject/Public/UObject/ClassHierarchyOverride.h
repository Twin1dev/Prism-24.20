#pragma once

/*=============================================================================
	ClassHierarchyOverride.h: installing one virtual override across a class tree.

	A UFunction override installed through FScriptFunctionOverride patches one
	virtual function table, because FVirtualFunctionOverride::Install refuses to
	run twice on the same object - IsInstalled() is the first thing it checks.
	That is the right behaviour for the object, but it means a hook on a base
	class only ever reaches the base class's own table: every native subclass
	carries its own copy of the table, and its copy still points at the original.

	Hooking a whole family therefore needs one FVirtualFunctionOverride per
	table, kept alive for as long as the hook should stay installed. This is
	that, as a single call.
=============================================================================*/

#include "CoreGlobals.h"

#include <algorithm>
#include <memory>
#include <vector>

PRISM_NAMESPACE_START

namespace UObjectHierarchy
{
	/**
	 * Installs Replacement into SlotIndex of the virtual function table of every class
	 * derived from BaseClass (BaseClass itself included), and reports the displaced
	 * implementation through OutOriginal.
	 *
	 * Returns the number of distinct tables patched.
	 *
	 * Three things this handles that a hand-written loop over the default objects does not:
	 *
	 *   * One override object per table. FVirtualFunctionOverride is non-copyable and
	 *     restores its slot when destroyed, so the objects are heap allocated and parked in
	 *     a static list. Reusing a single override - or, worse, holding a reference to one
	 *     that has already gone out of scope - silently patches only the first class, which
	 *     is exactly the failure this replaces.
	 *
	 *   * Deduplication by table, not by class. A Blueprint class does not get its own
	 *     virtual function table: it shares the table of its nearest native ancestor. So the
	 *     default objects of BP_Asteria_PIne_Tree_1_C and of its native parent give the same
	 *     table, and patching it twice would capture our own replacement as the "original"
	 *     and recurse forever the first time it was called.
	 *
	 *   * One original for the whole family. A subclass that carries its own implementation
	 *     in that slot holds a different function pointer, and patching it would quietly
	 *     redirect it to the base implementation, because there is only one OutOriginal to
	 *     hand back. Those are skipped with a warning rather than mis-hooked.
	 *
	 * SlotIndex is the index into the table, not a byte offset - divide what a disassembler
	 * shows by the pointer size.
	 */
	template<typename ClassType, typename ReturnType, typename... ArgTypes>
	inline int32 InstallVirtualOverrideForHierarchy(
		SDK::UClass* BaseClass,
		int32 SlotIndex,
		ReturnType (ClassType::*Replacement)(ArgTypes...),
		ReturnType (**OutOriginal)(ClassType*, ArgTypes...),
		const char* DebugName)
	{
		// Outlives the call: each entry restores its table slot when it is destroyed, so
		// dropping them here would uninstall the hook the moment this returned.
		static std::vector<std::unique_ptr<UE::Interception::FVirtualFunctionOverride>> InstalledOverrides;

		if (BaseClass == nullptr || SlotIndex < 0)
		{
			UE_LOG(LogPrism, Warning, __FUNCTION__": %s - no base class, or a negative slot index (%d). Nothing installed.",
				DebugName, SlotIndex);
			return 0;
		}

		void* const ReplacementAddress = SDK::FScriptFunctionOverride::ToFunctionAddress(Replacement);

		std::vector<void**> PatchedTables;
		void* SharedOriginal = nullptr;
		int32 NumInstalled = 0;
		int32 NumSkippedForOwnImplementation = 0;

		for (int32 Index = 0; Index < SDK::UObject::GObjects->Num(); ++Index)
		{
			SDK::UObject* Object = SDK::UObject::GObjects->GetByIndex(Index);
			if (Object == nullptr)
			{
				continue;
			}

			// The flag rather than a "Default__" name test: the name is a convention, the flag
			// is what the object system actually records.
			if ((static_cast<uint32>(Object->Flags) & static_cast<uint32>(SDK::EObjectFlags::ClassDefaultObject)) == 0)
			{
				continue;
			}

			if (!Object->IsA(BaseClass))
			{
				continue;
			}

			void** const VirtualTable = *reinterpret_cast<void***>(Object);
			if (VirtualTable == nullptr)
			{
				continue;
			}

			if (std::find(PatchedTables.begin(), PatchedTables.end(), VirtualTable) != PatchedTables.end())
			{
				continue;
			}

			void* const CurrentEntry = VirtualTable[SlotIndex];

			// Already ours - a second Initialize(), or a table reached by another route.
			if (CurrentEntry == ReplacementAddress)
			{
				PatchedTables.push_back(VirtualTable);
				continue;
			}

			if (SharedOriginal == nullptr)
			{
				SharedOriginal = CurrentEntry;
			}
			else if (CurrentEntry != SharedOriginal)
			{
				++NumSkippedForOwnImplementation;
				UE_LOG(LogPrism, Warning, __FUNCTION__": %s - %s has its own implementation in slot 0x%X (%p, not %p). "
					"Skipped: there is one original to hand back and hooking it would redirect it to the base's.",
					DebugName, Object->GetName().c_str(), SlotIndex, CurrentEntry, SharedOriginal);
				continue;
			}

			auto Override = std::make_unique<UE::Interception::FVirtualFunctionOverride>();
			if (!Override->Install(Object, SlotIndex, ReplacementAddress).IsValid())
			{
				UE_LOG(LogPrism, Warning, __FUNCTION__": %s - failed to install on %s.",
					DebugName, Object->GetName().c_str());
				continue;
			}

			PatchedTables.push_back(VirtualTable);
			InstalledOverrides.push_back(std::move(Override));
			++NumInstalled;
		}

		if (SharedOriginal == nullptr)
		{
			UE_LOG(LogPrism, Warning, __FUNCTION__": %s - found no default object deriving from %s. Nothing installed.",
				DebugName, BaseClass->GetName().c_str());
			return 0;
		}

		if (OutOriginal != nullptr)
		{
			*OutOriginal = reinterpret_cast<ReturnType (*)(ClassType*, ArgTypes...)>(SharedOriginal);
		}

		UE_LOG(LogPrism, Log, __FUNCTION__": %s - installed on %d virtual function table(s) under %s, original %p%s.",
			DebugName, NumInstalled, BaseClass->GetName().c_str(), SharedOriginal,
			NumSkippedForOwnImplementation > 0
				? (", " + std::to_string(NumSkippedForOwnImplementation) + " skipped for carrying their own implementation").c_str()
				: "");

		return NumInstalled;
	}
}

PRISM_NAMESPACE_END
