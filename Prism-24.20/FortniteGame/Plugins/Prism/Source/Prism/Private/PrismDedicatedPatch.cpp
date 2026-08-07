#include "pch.h"

#include "../Public/PrismDedicatedPatch.h"

class ProcessEventPatch : public UObject
{
public:
	UE_FUNCTION_OVERRIDE_BODY(ProcessEventPatch);

	static inline std::vector<std::string> LoggedFunctions;
	void* ProcessEvent(UFunction* Function, void* Params)
	{
		if (Function)
		{
			std::string FunctionName = Function->GetName();

			if (std::find(LoggedFunctions.begin(), LoggedFunctions.end(), FunctionName) == LoggedFunctions.end()) {
				std::cout << "ProcessEvent: " << this->GetName() << ": " << FunctionName << std::endl;
				LoggedFunctions.push_back(FunctionName);
			}

			return ProcessEvent_Original(this, Function, Params);
		}
	}
	UE_DECLARE_NATIVE_OVERRIDE(ProcessEvent);
};


void FPrismDedicatedPatch::Initialize()
{
	UE_LOG(LogPrism, Log, __FUNCTION__);

	ProcessEventPatch::ProcessEvent_Override.InstallAtRelativeAddress(0x00F86384);

	FImageMemory::Patch<bool>(0x10512933, false); // GIsClient
	FImageMemory::Patch<bool>(0x1075FF2C, true); // GIsServer

	FImageMemory::Null(0x31BEF70);
	FImageMemory::Null(0x1F2CD64);
	FImageMemory::Null(0xB830A74);

	FImageMemory::NullReturning(0x99AF8CC, 1);
	FImageMemory::NullReturning(0x21EE7FC, 0);

	FImageMemory::NullReturning(0x177CF8C, 1);
	//FImageMemory::NullReturning(0x8541E4C, 1);

	FImageMemory::Patch(0x21EE3F6, { 0x83, 0xF8, 0x01 });
}

IMPLEMENT_MODULE(FPrismDedicatedPatch);
