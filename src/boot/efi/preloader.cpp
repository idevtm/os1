// os1 EFI pre-loader: \EFI\BOOT\BOOTX64.EFI on the os1 USB stick.
//
// UEFI laptops have no serial port and the kernel cannot write the stick, so
// the kernel keeps its log in a reserved RAM window and resets on failure. On
// the next boot this pre-loader, while UEFI boot services still work:
//   1. finds a previous log in one of kBootLogUefiCandidates and saves it as
//      \os1logs\boot-NNNN.txt (plain text, readable on any computer);
//   2. appends one line per boot to \os1logs\journal.txt;
//   3. reserves a window (EfiReservedMemoryType) and writes a fresh header;
//   4. after saving a log, waits for a key so a crash loop cannot run unattended;
//   5. chain-loads Limine from \EFI\OS1\LIMINE.EFI.
// See doc/2026-11-02-real-hardware-bootlog.md.
#include "efi.hpp"

#include "debug/bootlog.hpp"
#include "handoff/memory_layout.h"

extern "C"
{
void* memset(void* destination, int value, size_t count)
{
    auto* bytes = static_cast<uint8_t*>(destination);
    for(size_t i = 0; i < count; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value);
    }
    return destination;
}

void* memcpy(void* destination, const void* source, size_t count)
{
    auto* to = static_cast<uint8_t*>(destination);
    const auto* from = static_cast<const uint8_t*>(source);
    for(size_t i = 0; i < count; ++i)
    {
        to[i] = from[i];
    }
    return destination;
}
}

namespace
{
constexpr const char16_t* kLimineImagePath = u"\\EFI\\OS1\\LIMINE.EFI";
constexpr const char16_t* kLogDirectory = u"\\os1logs";
constexpr const char16_t* kJournalName = u"journal.txt";
constexpr uint64_t kWindowPages = kBootLogUefiWindowBytes / 0x1000;
constexpr size_t kCandidateCount = sizeof(kBootLogUefiCandidates) / sizeof(kBootLogUefiCandidates[0]);

EFI_SYSTEM_TABLE* g_system_table = nullptr;
EFI_BOOT_SERVICES* g_boot_services = nullptr;
alignas(8) uint8_t g_memory_map[32768];
uint64_t g_memory_map_size = 0;
uint64_t g_descriptor_size = 0;
uint8_t g_read_buffer[4096];

// Small ASCII text builder for screen, journal, and log-file headers.
class Text
{
public:
    Text& add(const char* text)
    {
        while((nullptr != text) && ('\0' != *text) && (length_ + 1 < sizeof(data_)))
        {
            data_[length_++] = *text++;
        }
        data_[length_] = '\0';
        return *this;
    }

    Text& hex(uint64_t value)
    {
        char digits[17];
        for(int i = 15; i >= 0; --i)
        {
            digits[i] = "0123456789abcdef"[value & 0xF];
            value >>= 4;
        }
        digits[16] = '\0';
        add("0x");
        return add(digits);
    }

    Text& dec(uint64_t value, int minimum_digits = 1)
    {
        char digits[21];
        int count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while((0 != value) || (count < minimum_digits));
        char ordered[21];
        for(int i = 0; i < count; ++i)
        {
            ordered[i] = digits[count - 1 - i];
        }
        ordered[count] = '\0';
        return add(ordered);
    }

    [[nodiscard]] const char* c_str() const
    {
        return data_;
    }

    [[nodiscard]] size_t length() const
    {
        return length_;
    }

private:
    char data_[512] = {};
    size_t length_ = 0;
};

void print(const char* text)
{
    char16_t wide[256];
    size_t used = 0;
    auto flush = [&]() {
        wide[used] = 0;
        g_system_table->con_out->output_string(g_system_table->con_out, wide);
        used = 0;
    };
    for(; '\0' != *text; ++text)
    {
        if(used + 3 >= sizeof(wide) / sizeof(wide[0]))
        {
            flush();
        }
        if('\n' == *text)
        {
            wide[used++] = u'\r';
        }
        wide[used++] = static_cast<char16_t>(static_cast<uint8_t>(*text));
    }
    flush();
}

void wait_for_key()
{
    uint64_t index = 0;
    g_boot_services->wait_for_event(1, &g_system_table->con_in->wait_for_key, &index);
    EFI_INPUT_KEY key{};
    g_system_table->con_in->read_key_stroke(g_system_table->con_in, &key);
}

bool load_memory_map()
{
    uint64_t key = 0;
    uint32_t version = 0;
    g_memory_map_size = sizeof(g_memory_map);
    return !efi_error(g_boot_services->get_memory_map(
        &g_memory_map_size, reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(g_memory_map), &key,
        &g_descriptor_size, &version));
}

// True when `address` lies in RAM (not MMIO or a firmware hole), so reading a
// possible stale log header there is safe.
bool address_is_ram(uint64_t address)
{
    for(uint64_t offset = 0; offset + g_descriptor_size <= g_memory_map_size; offset += g_descriptor_size)
    {
        const auto* descriptor = reinterpret_cast<const EFI_MEMORY_DESCRIPTOR*>(g_memory_map + offset);
        const uint64_t end = descriptor->physical_start + descriptor->number_of_pages * 0x1000;
        if((address >= descriptor->physical_start) && (address < end))
        {
            return (descriptor->type >= EfiLoaderCode) && (descriptor->type <= EfiConventionalMemory);
        }
    }
    return false;
}

bool window_is_ram(uint64_t window)
{
    return address_is_ram(window) && address_is_ram(window + kBootLogUefiWindowBytes - 1);
}

bool header_valid(const BootLogHeader* log)
{
    return (kBootLogMagic == log->magic) && (kBootLogVersion == log->version) &&
           (kBootLogHeaderBytes == log->header_bytes) &&
           (log->capacity == (kBootLogUefiWindowBytes - kBootLogHeaderBytes));
}

void append_flag_names(Text& text, uint32_t flags)
{
    struct FlagName
    {
        uint32_t bit;
        const char* name;
    };
    const FlagName names[] = {
        {kBootLogFlagKernelAttached, " kernel-attached"},
        {kBootLogFlagUserInitStarted, " user-init-started"},
        {kBootLogFlagFatalStop, " fatal-stop"},
        {kBootLogFlagWatchdogArmed, " watchdog-armed"},
        {kBootLogFlagKernelMainReturned, " kernel_main-returned"},
    };
    for(const FlagName& entry : names)
    {
        if(0 != (flags & entry.bit))
        {
            text.add(entry.name);
        }
    }
}

bool write_all(EFI_FILE_PROTOCOL* file, const void* data, uint64_t length)
{
    uint64_t written = length;
    return !efi_error(file->write(file, &written, data)) && (written == length);
}

void make_name(char16_t* name, const char* prefix, uint64_t number, const char* suffix)
{
    Text text;
    text.add(prefix).dec(number, 4).add(suffix);
    size_t i = 0;
    for(; '\0' != text.c_str()[i]; ++i)
    {
        name[i] = static_cast<char16_t>(text.c_str()[i]);
    }
    name[i] = 0;
}

// Count journal lines so each boot gets a stable number across reboots.
uint64_t count_journal_lines(EFI_FILE_PROTOCOL* journal)
{
    uint64_t lines = 0;
    for(;;)
    {
        uint64_t size = sizeof(g_read_buffer);
        if(efi_error(journal->read(journal, &size, g_read_buffer)) || (0 == size))
        {
            return lines;
        }
        for(uint64_t i = 0; i < size; ++i)
        {
            lines += ('\n' == g_read_buffer[i]) ? 1u : 0u;
        }
    }
}

bool save_log(EFI_FILE_PROTOCOL* directory, uint64_t window, const BootLogHeader* log,
              uint64_t log_number, char16_t* file_name)
{
    make_name(file_name, "boot-", log_number, ".txt");
    EFI_FILE_PROTOCOL* file = nullptr;
    if(efi_error(directory->open(directory, &file, file_name,
                                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
    {
        return false;
    }

    Text header;
    header.add("os1 boot log from boot #").dec(log->boot_sequence).add(", window ").hex(window);
    header.add(", ").dec(log->write_count).add(" bytes, flags:");
    append_flag_names(header, log->flags);
    if(log->write_count > log->capacity)
    {
        header.add(" (oldest ").dec(log->write_count - log->capacity).add(" bytes overwritten)");
    }
    header.add("\n----\n");

    const auto* ring = reinterpret_cast<const uint8_t*>(window + kBootLogHeaderBytes);
    bool ok = write_all(file, header.c_str(), header.length());
    if(log->write_count <= log->capacity)
    {
        ok = ok && write_all(file, ring, log->write_count);
    }
    else
    {
        const uint64_t begin = log->write_count % log->capacity;
        ok = ok && write_all(file, ring + begin, log->capacity - begin) && write_all(file, ring, begin);
    }
    file->close(file);
    return ok;
}

void append_to_window(BootLogHeader* log, const char* text)
{
    auto* ring = reinterpret_cast<char*>(log + 1);
    for(; '\0' != *text; ++text)
    {
        ring[log->write_count++ % log->capacity] = *text;
    }
}

EFI_STATUS chain_load_limine(EFI_HANDLE image, EFI_LOADED_IMAGE_PROTOCOL* loaded_image)
{
    EFI_DEVICE_PATH_PROTOCOL* device_path = nullptr;
    EFI_STATUS status = g_boot_services->handle_protocol(
        loaded_image->device_handle, &kDevicePathProtocolGuid, reinterpret_cast<void**>(&device_path));
    if(efi_error(status))
    {
        return status;
    }

    uint64_t device_path_bytes = 0;
    for(const auto* node = reinterpret_cast<const uint8_t*>(device_path); 0x7F != node[0];)
    {
        const uint16_t node_length = static_cast<uint16_t>(node[2] | (node[3] << 8));
        device_path_bytes += node_length;
        node += node_length;
    }

    uint64_t path_units = 0;
    while(0 != kLimineImagePath[path_units])
    {
        ++path_units;
    }
    const uint64_t file_node_bytes = 4 + (path_units + 1) * 2;
    uint8_t* full_path = nullptr;
    status = g_boot_services->allocate_pool(EfiLoaderData, device_path_bytes + file_node_bytes + 4,
                                            reinterpret_cast<void**>(&full_path));
    if(efi_error(status))
    {
        return status;
    }

    memcpy(full_path, device_path, device_path_bytes);
    uint8_t* file_node = full_path + device_path_bytes;
    file_node[0] = 0x04;  // Media device path
    file_node[1] = 0x04;  // File path
    file_node[2] = static_cast<uint8_t>(file_node_bytes & 0xFF);
    file_node[3] = static_cast<uint8_t>(file_node_bytes >> 8);
    memcpy(file_node + 4, kLimineImagePath, (path_units + 1) * 2);
    uint8_t* end_node = file_node + file_node_bytes;
    end_node[0] = 0x7F;
    end_node[1] = 0xFF;
    end_node[2] = 4;
    end_node[3] = 0;

    EFI_HANDLE limine = nullptr;
    status = g_boot_services->load_image(false, image, reinterpret_cast<EFI_DEVICE_PATH_PROTOCOL*>(full_path),
                                         nullptr, 0, &limine);
    if(efi_error(status))
    {
        return status;
    }
    return g_boot_services->start_image(limine, nullptr, nullptr);
}
}  // namespace

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* system_table)
{
    g_system_table = system_table;
    g_boot_services = system_table->boot_services;
    // The firmware's 5-minute boot watchdog would reset us while waiting for a key.
    g_boot_services->set_watchdog_timer(0, 0, 0, nullptr);
    print("[os1-bootlog] pre-loader\n");

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = nullptr;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* file_system = nullptr;
    EFI_FILE_PROTOCOL* root = nullptr;
    EFI_FILE_PROTOCOL* directory = nullptr;
    EFI_FILE_PROTOCOL* journal = nullptr;
    bool files_ready =
        !efi_error(g_boot_services->handle_protocol(image, &kLoadedImageProtocolGuid,
                                                    reinterpret_cast<void**>(&loaded_image))) &&
        !efi_error(g_boot_services->handle_protocol(loaded_image->device_handle, &kSimpleFileSystemProtocolGuid,
                                                    reinterpret_cast<void**>(&file_system))) &&
        !efi_error(file_system->open_volume(file_system, &root)) &&
        !efi_error(root->open(root, &directory, kLogDirectory,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                              EFI_FILE_DIRECTORY)) &&
        !efi_error(directory->open(directory, &journal, kJournalName,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0));
    if(!files_ready)
    {
        print("[os1-bootlog] WARNING: cannot open \\os1logs on the boot volume; logs will not be saved\n");
    }

    const uint64_t boot_number = (files_ready ? count_journal_lines(journal) : 0) + 1;
    const bool memory_map_ready = load_memory_map();

    // 1. Save the previous boot's log, if RAM kept one.
    Text journal_line;
    journal_line.add("boot ").dec(boot_number, 4).add(":");
    uint64_t harvested_window = 0;
    bool saved = false;
    char16_t saved_name[32] = {};
    for(size_t i = 0; memory_map_ready && (i < kCandidateCount); ++i)
    {
        const uint64_t window = kBootLogUefiCandidates[i];
        if(!window_is_ram(window))
        {
            journal_line.add(" w").dec(i).add("=not-ram");
            continue;
        }
        const auto* log = reinterpret_cast<const BootLogHeader*>(window);
        journal_line.add(" w").dec(i).add("=").hex(log->magic);
        if((0 != harvested_window) || !header_valid(log))
        {
            continue;
        }
        harvested_window = window;
        journal_line.add("(boot#").dec(log->boot_sequence).add(" bytes=").dec(log->write_count);
        journal_line.add(" flags:");
        append_flag_names(journal_line, log->flags);
        journal_line.add(")");
        const uint64_t log_number = ((0 != log->boot_sequence) && (log->boot_sequence < boot_number))
                                        ? log->boot_sequence
                                        : boot_number - 1;
        saved = files_ready && save_log(directory, window, log, log_number, saved_name);
        journal_line.add(saved ? " saved" : " SAVE-FAILED");
    }
    if(0 == harvested_window)
    {
        journal_line.add(" no previous log");
    }

    // 2. Arm a fresh window for this boot. Allocating it as reserved memory keeps
    // Limine and the kernel's page allocator away from it.
    BootLogHeader* armed = nullptr;
    for(size_t i = 0; i < kCandidateCount; ++i)
    {
        EFI_PHYSICAL_ADDRESS window = kBootLogUefiCandidates[i];
        if(efi_error(g_boot_services->allocate_pages(AllocateAddress, EfiReservedMemoryType, kWindowPages,
                                                     &window)))
        {
            continue;
        }
        armed = reinterpret_cast<BootLogHeader*>(window);
        memset(armed, 0, kBootLogHeaderBytes);
        armed->magic = kBootLogMagic;
        armed->version = kBootLogVersion;
        armed->header_bytes = kBootLogHeaderBytes;
        armed->capacity = kBootLogUefiWindowBytes - kBootLogHeaderBytes;
        armed->boot_sequence = static_cast<uint32_t>(boot_number);
        break;
    }
    // Make sure a harvested log is not saved twice when it sits outside the
    // window armed above.
    if((0 != harvested_window) && (reinterpret_cast<uint64_t>(armed) != harvested_window))
    {
        EFI_PHYSICAL_ADDRESS stale = harvested_window;
        if(!efi_error(g_boot_services->allocate_pages(AllocateAddress, EfiLoaderData, kWindowPages, &stale)))
        {
            reinterpret_cast<BootLogHeader*>(stale)->magic = 0;
            g_boot_services->free_pages(stale, kWindowPages);
        }
    }
    if(nullptr != armed)
    {
        journal_line.add(" armed=").hex(reinterpret_cast<uint64_t>(armed));
        Text banner;
        banner.add("[os1-bootlog] boot #").dec(boot_number).add(" armed window ").hex(reinterpret_cast<uint64_t>(armed));
        banner.add("\n");
        append_to_window(armed, banner.c_str());
    }
    else
    {
        journal_line.add(" ARM-FAILED");
    }
    journal_line.add("\n");

    if(files_ready)
    {
        journal->set_position(journal, ~0ull);
        write_all(journal, journal_line.c_str(), journal_line.length());
        journal->close(journal);
        directory->close(directory);
        root->close(root);
    }
    print(journal_line.c_str());

    // 3. Stop after a saved crash log so a reset loop cannot run unattended.
    if(saved)
    {
        print("[os1-bootlog] previous boot log saved to \\os1logs\\");
        char ascii_name[32];
        size_t i = 0;
        for(; 0 != saved_name[i]; ++i)
        {
            ascii_name[i] = static_cast<char>(saved_name[i]);
        }
        ascii_name[i] = '\0';
        print(ascii_name);
        print("\n[os1-bootlog] press any key to boot again, or power off and collect the stick\n");
        wait_for_key();
    }

    const EFI_STATUS status = chain_load_limine(image, loaded_image);
    Text failure;
    failure.add("[os1-bootlog] starting \\EFI\\OS1\\LIMINE.EFI failed, status ").hex(status).add("\n");
    print(failure.c_str());
    wait_for_key();
    return status;
}
