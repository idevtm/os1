#pragma once
struct Process;
class PageFrameContainer;
bool usb_start_worker(Process*, PageFrameContainer&);

void usb_queue_console_char(char);
void usb_poll_console();
