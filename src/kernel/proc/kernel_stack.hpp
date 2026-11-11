#pragma once

#include <stdint.h>

class PageFrameContainer;

bool initialize_kernel_stacks(PageFrameContainer& frames, uint64_t kernel_root);
bool allocate_kernel_stack(PageFrameContainer& frames, uint64_t& physical_base, uint64_t& top);
void release_kernel_stack(PageFrameContainer& frames, uint64_t physical_base);
