#pragma once
#include <os1/resource.h>
#include <os1/vfs.h>
#include <stddef.h>

#include "sync/smp.hpp"

struct NativeObject
{
    volatile uint32_t references;
    uint32_t kind;
    uint32_t interfaces;
    uint32_t allowed_rights;
    void (*destroy)(NativeObject*);
    // Console-stage byte providers; reservation is bounded and must be finished.
    bool (*reserve_read)(char*, size_t, size_t&, long*) = nullptr;
    void (*finish_read)(size_t, bool) = nullptr;
    void (*write_bytes)(const char*, size_t) = nullptr;
    long (*acquire_open)(NativeObject*) = nullptr;
    void (*release_open)(NativeObject*) = nullptr;
};
struct OpenInstance
{
    volatile uint32_t references;
    NativeObject* object;
    volatile uint32_t status_flags;
    volatile bool cursor_busy;
    // Files use a byte cursor. Buffers use a borrowed authority generation.
    // Buffer instances cannot use byte I/O, seek, or directory operations.
    uint64_t cursor;
};
struct DescriptorEntry
{
    OpenInstance* instance = nullptr;
    uint32_t rights = 0;
    uint32_t flags = 0;
    uint32_t descendant_rights = 0;
};
struct DescriptorTable
{
    Spinlock lock{"descriptors"};
    bool closed = false;
    DescriptorEntry entries[OS1_DESCRIPTOR_LIMIT]{};
};
void object_retain(NativeObject* object);
void object_release(NativeObject* object);
OpenInstance* open_instance(NativeObject* object, long* error = nullptr);
void instance_retain(OpenInstance* instance);
void instance_release(OpenInstance* instance);
void descriptor_table_initialize(DescriptorTable& table);
// install retains the supplied instance; caller retains ownership of its reference.
long descriptor_install(DescriptorTable& table,
                        OpenInstance* instance,
                        uint32_t rights,
                        uint32_t flags,
                        int target = -1,
                        uint32_t descendant_rights = 0);
long descriptor_acquire(DescriptorTable& table, int fd, uint32_t rights, OpenInstance*& instance);
long descriptor_close(DescriptorTable& table, int fd);
long descriptor_duplicate(DescriptorTable& table, int fd, const os1_duplicate_request& request);
long descriptor_query(DescriptorTable& table, int fd, os1_resource_info& result);
long descriptor_set_flags(DescriptorTable& table, int fd, uint32_t flags);
long descriptor_set_status(DescriptorTable& table, int fd, uint32_t flags);
void descriptor_close_all(DescriptorTable& table);
void descriptor_close_exec(DescriptorTable& table);
// Console-stage compatibility grant map: exactly existing 0/1/2, identical rights,
// independent child flags cleared for the child's initial exec. Other slots excluded.
long descriptor_inherit_standard(DescriptorTable& parent, DescriptorTable& child);
long descriptor_bootstrap_console(DescriptorTable& table);

class InstanceRef
{
public:
    explicit InstanceRef(OpenInstance* value = nullptr) : value_(value) {}
    ~InstanceRef()
    {
        instance_release(value_);
    }
    InstanceRef(const InstanceRef&) = delete;
    InstanceRef& operator=(const InstanceRef&) = delete;
    OpenInstance* get() const
    {
        return value_;
    }
    OpenInstance* take()
    {
        auto* value = value_;
        value_ = nullptr;
        return value;
    }

private:
    OpenInstance* value_;
};

long descriptor_acquire_entry(DescriptorTable&, int, uint32_t, DescriptorEntry&);
long descriptor_duplicate_scoped(DescriptorTable&, int, const os1_scoped_duplicate_request&);
