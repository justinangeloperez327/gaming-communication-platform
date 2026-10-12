#pragma once

namespace gcp::probe {
void list_audio_devices();
int run_microphone_probe(int seconds);
}
