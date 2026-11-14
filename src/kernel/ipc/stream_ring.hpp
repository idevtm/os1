#pragma once
#include <os1/ipc.h>
#include <stdint.h>

// The ring of a byte stream. The kernel protects it with the IPC wait lock. This file has no
// kernel dependency, so that a host test can use it.
struct StreamRing
{
    uint8_t* bytes;        // OS1_STREAM_BYTES bytes.
    uint32_t head, count;  // Index of the oldest byte, and the number of bytes.
    bool reader_closed, writer_closed;

    // Add bytes at the end. Return the number of bytes that the ring had space for.
    uint32_t put(const uint8_t* source, uint32_t wanted)
    {
        uint32_t added = 0;
        for(; added < wanted && count < OS1_STREAM_BYTES; ++added, ++count)
            bytes[(head + count) % OS1_STREAM_BYTES] = source[added];
        return added;
    }
    // Remove the oldest bytes. Return the number of bytes that the ring had.
    uint32_t take(uint8_t* target, uint32_t wanted)
    {
        uint32_t taken = 0;
        for(; taken < wanted && count; ++taken, --count)
        {
            target[taken] = bytes[head];
            head = (head + 1) % OS1_STREAM_BYTES;
        }
        return taken;
    }
    // A reader does not wait if the ring has bytes or the writer end is closed.
    bool reader_ready() const
    {
        return count || writer_closed;
    }
    // A writer does not wait if the ring has space or the reader end is closed.
    bool writer_ready() const
    {
        return count < OS1_STREAM_BYTES || reader_closed;
    }
};
