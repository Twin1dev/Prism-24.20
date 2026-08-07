#include "pch.h"

#include "Engine/Restricted/NotForLicensees/Plugins/OnlineGameplayFramework/Source/McpProfileSys/Public/McpProfileGroup.h"

PRISM_NAMESPACE_START

void UMcpProfileGroup::SendRequestNow(__int64* HttpRequest, EContextCredentials ContextCredentials)
{
	return SendRequestNow_Original(this, HttpRequest, CXC_Public);
}

void UMcpProfileGroup::Initialize()
{
	SendRequestNow_Override.InstallAtRelativeAddress(0x162F7C0);
}

IMPLEMENT_MODULE(UMcpProfileGroup);

PRISM_NAMESPACE_END