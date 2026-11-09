#pragma once
#include <os1/log.h>
struct Process;
struct Thread;
class PageFrameContainer;
namespace diagnostic_log
{
void initialize(bool diagnostic);
void tick(uint64_t elapsed_ns);
#ifdef OS1_HOST_TEST
void test_reset();
#endif
void text(char c);
void event(uint32_t type, uint32_t flags, uint64_t pid, uint64_t a, uint64_t b);
void append(os1_log_record record);
os1_log_record origin(Process*, uint32_t kind);
void inherit(Process* child, const Process* parent);
void cleanup(Process*);
void process_exec(Process*);
void process_exit(Process*, int status);
void syscall_begin(Thread*, uint64_t number, uint64_t operation);
void syscall_end(Thread*, int64_t status);
void ipc_begin(os1_log_record& record, Process*, uint64_t interface_id, uint32_t member);
void ipc_end(const os1_log_record&, int64_t status);
long read(const os1_log_query&, os1_log_record&);
os1_log_info info();
long context(PageFrameContainer&, Thread*, uint64_t operation, uint64_t pointer);
}  // namespace diagnostic_log

namespace diagnostic_log
{
struct HistoryState
{
    uint64_t session, latest, overwritten;
    bool closing;
};
uint64_t monotonic();
uint64_t error_sequence();
void enable_export(bool enabled);
bool history_state(unsigned slot, HistoryState&);
long history_read(unsigned slot, uint64_t session, const os1_log_query&, os1_log_history_record&);
void retire_history(unsigned slot, uint64_t session);
}  // namespace diagnostic_log
