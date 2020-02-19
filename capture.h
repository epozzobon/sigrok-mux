#pragma once

void capture_init();
void capture_run();
bool capture_stop();
void capture_cleanup();
extern void on_capture_change(double time, uint64_t prev, uint64_t value);


