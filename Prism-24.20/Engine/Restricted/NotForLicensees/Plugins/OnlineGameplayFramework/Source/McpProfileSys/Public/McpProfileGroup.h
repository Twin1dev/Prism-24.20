#pragma once

#include "CoreGlobals.h"

enum EContextCredentials : int32
{
    CXC_Client = 0x0,
    CXC_DedicatedServer = 0x1,
    CXC_Cheater = 0x2,
    CXC_Public = 0x3,
};

PRISM_NAMESPACE_START

class UMcpProfileGroup : public UE_EXTENDS_SDK_CLASS(UMcpProfileGroup)
{
public:
    UE_FUNCTION_OVERRIDE_BODY(UMcpProfileGroup)

    void SendRequestNow(__int64* HttpRequest, EContextCredentials ContextCredentials);
    UE_DECLARE_NATIVE_OVERRIDE(SendRequestNow);

    static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(UMcpProfileGroup);
UE_DECLARE_PRISM_TYPE(UMcpProfileGroup);

PRISM_NAMESPACE_END