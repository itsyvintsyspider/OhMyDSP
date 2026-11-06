#include "UserClient.hpp"
#include "OhMyDSP.hpp"
#include "Capture.hpp"
#include <os/log.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(OhMyDSPUserClient, IOUserClient)
#undef super

bool OhMyDSPUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties)
{
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "OhMyDSP: user client refused (not root)");
        return false;
    }
    return IOUserClient::initWithTask(owningTask, securityToken, type, properties);
}

bool OhMyDSPUserClient::start(IOService *provider)
{
    owner = OSDynamicCast(OhMyDSP, provider);
    if (!owner || !owner->capture() || !IOUserClient::start(provider)) return false;
    if (!owner->claimClient(this)) { os_log(OS_LOG_DEFAULT, "OhMyDSP: user client refused (already in use)"); return false; }
    opened = true;
    os_log(OS_LOG_DEFAULT, "OhMyDSP: user client attached");
    return true;
}

void OhMyDSPUserClient::stop(IOService *provider)
{
    if (opened && owner) { owner->capture()->stop(); owner->releaseClient(this); opened = false; }   // a vanished client must not leave the DSP powered
    IOUserClient::stop(provider);
}

IOReturn OhMyDSPUserClient::clientClose()
{
    os_log(OS_LOG_DEFAULT, "OhMyDSP: user client closed");
    if (opened && owner) { owner->capture()->stop(); owner->releaseClient(this); opened = false; }
    terminate();
    return kIOReturnSuccess;
}

IOReturn OhMyDSPUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (!opened || !owner || !owner->capture()) return kIOReturnNotAttached;
    IOMemoryDescriptor *m = type == OHMYDSP_MEM_RING ? owner->capture()->ringMemory() : type == OHMYDSP_MEM_STATUS ? owner->capture()->statusMemory() : nullptr;
    if (!m) return kIOReturnBadArgument;
    m->retain(); *memory = m; *options = kIOMapReadOnly;
    return kIOReturnSuccess;
}

IOReturn OhMyDSPUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!opened || !owner || !owner->capture()) return kIOReturnNotAttached;
    switch (selector) {
    case OHMYDSP_SEL_START: return owner->capture()->start();
    case OHMYDSP_SEL_STOP:  return owner->capture()->stop();
    case OHMYDSP_SEL_TUNE:
        if (!args || args->scalarInputCount < 2) return kIOReturnBadArgument;
        return owner->capture()->tune((uint32_t)args->scalarInput[0], args->scalarInput[1]);
    }
    return kIOReturnUnsupported;
}
