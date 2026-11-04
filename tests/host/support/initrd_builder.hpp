#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "elf/elf64.hpp"
#include "handoff/memory_layout.h"
namespace os1::host_test
{
struct InitrdBuilder
{
    std::vector<uint8_t> bytes;
    void entry(const std::string& name,
               const std::string& data,
               uint32_t mode = 0100644,
               uint32_t links = 1)
    {
        bytes.insert(bytes.end(), {'0', '7', '0', '7', '0', '1'});
        const uint32_t fields[] = {1,
                                   mode,
                                   0,
                                   0,
                                   links,
                                   0,
                                   static_cast<uint32_t>(data.size()),
                                   0,
                                   0,
                                   0,
                                   0,
                                   static_cast<uint32_t>(name.size() + 1),
                                   0};
        for(auto field : fields)
        {
            char hex[9];
            std::snprintf(hex, sizeof(hex), "%08x", field);
            bytes.insert(bytes.end(), hex, hex + 8);
        }
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0);
        while(bytes.size() % 4)
            bytes.push_back(0);
        bytes.insert(bytes.end(), data.begin(), data.end());
        while(bytes.size() % 4)
            bytes.push_back(0);
    }
    void finish()
    {
        entry("TRAILER!!!", {}, 0);
    }
};
inline std::string tiny_executable()
{
    std::string data(0x200, '\0');
    auto* header = reinterpret_cast<elf::Elf64Header*>(data.data());
    header->magic = elf::kMagic;
    header->type = elf::kTypeExec;
    header->machine = elf::kMachineX86_64;
    header->entry = kUserImageBase;
    header->phoff = sizeof(*header);
    header->ehsize = sizeof(*header);
    header->phentsize = sizeof(elf::Elf64ProgramHeader);
    header->phnum = 1;
    auto* segment = reinterpret_cast<elf::Elf64ProgramHeader*>(data.data() + header->phoff);
    segment->type = elf::kProgramTypeLoad;
    segment->flags = elf::kProgramFlagExecute;
    segment->offset = 0x100;
    segment->vaddr = kUserImageBase;
    segment->filesz = 4;
    segment->memsz = kPageSize;
    data[0x100] = '\xcc';
    return data;
}
}  // namespace os1::host_test
