#include "Modules/ModuleManager.h"
#include "RTFWUnrealWorldHost.h"
#include "RTFWUnrealAdapters.h"
#include <exception>

class FRTFWUnrealModule final : public IModuleInterface
{
public:
    bool SupportsDynamicReloading() override {
        return FRTFWUnrealWorldHost::ActiveHosts() == 0 && RTFWUnrealActiveAdapters() == 0;
    }
    void ShutdownModule() override {
        if (!SupportsDynamicReloading()) std::terminate();
    }
};

IMPLEMENT_MODULE(FRTFWUnrealModule, RTFWUnreal)
