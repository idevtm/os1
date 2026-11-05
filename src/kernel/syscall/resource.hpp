#pragma once
#include "mm/page_frame.hpp"
#include "proc/thread.hpp"
long sys_resource(
    PageFrameContainer& frames, Thread* thread, uint64_t fd, uint64_t operation, uint64_t argument);
