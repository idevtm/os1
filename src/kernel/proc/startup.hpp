#pragma once
#include <os1/startup.h>

#include "proc/thread.hpp"
#include "syscall/process.hpp"

class LegacyStartupRecord
{
public:
    LegacyStartupRecord(const Process& process, const char* path);
    ~LegacyStartupRecord();

    LegacyStartupRecord(const LegacyStartupRecord&) = delete;
    LegacyStartupRecord& operator=(const LegacyStartupRecord&) = delete;

    const os1_startup_record* get() const
    {
        return record_;
    }

private:
    os1_startup_record* record_ = nullptr;
};

long validate_startup(const os1_startup_record&);
long validate_namespace_roles(DescriptorTable&, int root, int cwd, bool exec);
long install_child_grants(DescriptorTable&, DescriptorTable&, const os1_launch_request&);
long resolve_process_image(Process*, const char* path, OpenInstance*& image);
bool install_startup_record(PageFrameContainer&,
                            uint64_t cr3,
                            const os1_startup_record&,
                            uint64_t& rsp,
                            uint64_t& argument);
long sys_launch(const ProcessSyscallContext&, uint64_t executable, uint64_t mode, uint64_t request);
long replace_process_image(const ProcessSyscallContext&,
                           Thread*,
                           OpenInstance*,
                           const os1_startup_record&,
                           const char* name);
long inherit_process_namespace(Process*, Process*);
