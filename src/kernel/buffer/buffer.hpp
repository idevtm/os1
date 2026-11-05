#pragma once
#include <os1/buffer.h>
#include <stdint.h>
class PageFrameContainer;
struct Thread;
struct Process;
struct os1_ipc_buffers;
struct BufferLoanBatch;
struct BufferOutputBatch;
long sys_buffer(PageFrameContainer&, Thread*, uint64_t, uint64_t, uint64_t);
long buffer_prepare_loans(PageFrameContainer&, Thread*, const os1_ipc_buffers&, BufferLoanBatch*&);
void buffer_cancel_loans(BufferLoanBatch*);
long buffer_receive_loans(BufferLoanBatch*, Thread*, os1_ipc_buffers&);
void buffer_rollback_receive(BufferLoanBatch*, Thread*, const os1_ipc_buffers&);
void buffer_finish_loans(BufferLoanBatch*);
void buffer_release_loans(BufferLoanBatch*);
long buffer_prepare_outputs(
    PageFrameContainer&, Thread*, BufferLoanBatch*, const os1_ipc_buffers&, BufferOutputBatch*&);
void buffer_cancel_outputs(BufferOutputBatch*);
long buffer_claim_outputs(BufferOutputBatch*, Thread*, os1_ipc_buffers&);
void buffer_rollback_claim(BufferOutputBatch*, Thread*, const os1_ipc_buffers&);
void buffer_commit_claim(BufferOutputBatch*);
void buffer_release_outputs(BufferOutputBatch*);
void buffer_process_cleanup(PageFrameContainer&, Process*);
void buffer_process_exec(PageFrameContainer&, Process*);
