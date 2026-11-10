#pragma once
#include "hardware/catalog.hpp"
struct BlockDevice;
namespace hardware_catalog
{
unsigned add(unsigned kind, const os1::hardware::Record&);
os1::hardware::Record* edit(unsigned id);
unsigned usb_port(unsigned pci, unsigned port, bool connected);
unsigned usb_device(unsigned parent, unsigned root, unsigned port, unsigned speed_mbps_x10);
void usb_descriptor(unsigned id, const void* bytes, unsigned length);
void usb_configuration(unsigned id,
                       const void* bytes,
                       unsigned length,
                       bool descriptor_valid = true);
unsigned usb_bind(unsigned id, unsigned config, unsigned number, const char* driver, bool ready);
void unavailable(unsigned id);
void block(BlockDevice&, unsigned backing);
void partitions(BlockDevice&, const uint8_t* mbr);
void mounted(BlockDevice&, unsigned partition, const uint8_t* super);
}  // namespace hardware_catalog
