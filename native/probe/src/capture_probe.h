#pragma once

namespace gcp::probe {
void list_monitors();
int run_monitor_probe(int index, int seconds);
}
