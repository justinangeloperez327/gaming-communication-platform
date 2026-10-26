#pragma once
#include "screen_control.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <thread>
#include <algorithm>

namespace gcp::transport {
template<class Control> class Input final {
public:
    explicit Input(Control& control) : control_(control), reader_([this] { read(); }) {}
    ~Input() {
        cancelled_.store(true);
        if (reader_.joinable()) reader_.join();
    }
private:
    void read() {
        std::array<char, 1024> bytes{};
        std::string line;
        while (!cancelled_.load()) {
            DWORD available = 0;
            const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
            DWORD size = 0;
            if (!ReadFile(pipe, bytes.data(),
                          (std::min)(available, static_cast<DWORD>(bytes.size())), &size, nullptr) || !size) break;
            for (DWORD i = 0; i < size; ++i) {
                if (bytes[i] == '\n') {
                    if (!control_.accept(line, screen_now_ms())) { control_.stop(); return; }
                    line.clear();
                } else {
                    if (line.size() >= 12288 || bytes[i] == '\0' || bytes[i] == '\r') {
                        control_.stop(); return;
                    }
                    line += bytes[i];
                }
            }
        }
        control_.stop();
    }
    Control& control_;
    std::atomic<bool> cancelled_{false};
    std::thread reader_;
};
} // namespace gcp::transport
