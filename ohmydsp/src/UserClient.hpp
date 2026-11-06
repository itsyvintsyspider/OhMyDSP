// Phase 6.1: userspace access to the capture session. Root only, one client at a time (see include/OhMyDSPShared.h for the contract).
#pragma once
#include <IOKit/IOUserClient.h>

class OhMyDSP;

class OhMyDSPUserClient : public IOUserClient {
    OSDeclareDefaultStructors(OhMyDSPUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
private:
    OhMyDSP *owner = nullptr;
    bool opened = false;
};
