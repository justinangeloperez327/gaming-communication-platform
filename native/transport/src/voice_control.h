#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace gcp::transport {

// One owning thread (the audio session loop). No worker thread, detached stdin
// reader, audio buffer, or unbounded command queue is needed.
class VoiceControl final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto lease_timeout = std::chrono::seconds{5};
    static constexpr std::size_t max_command_bytes = 32;

    VoiceControl(bool consent, bool managed, bool push_to_talk, TimePoint now)
        : consent_(consent), managed_(managed), push_to_talk_(push_to_talk),
          muted_(managed), heartbeat_(now) {}

    void receive(std::string_view bytes, TimePoint now) {
        if (stopped(now)) { stop(); return; }
        for (const char byte : bytes) {
            if (stopped_) return;
            if (byte == '\n') {
                command(pending_, now);
                pending_.clear();
            } else if (pending_.size() >= max_command_bytes || byte < ' ' || byte > '~') {
                stop();
            } else {
                pending_.push_back(byte);
            }
        }
    }

    [[nodiscard]] bool takeMuteRequest() noexcept {
        return std::exchange(mute_requested_, false);
    }
    void resetPushToTalk() noexcept { key_released_ = false; }
    void stop() noexcept { stopped_ = true; muted_ = true; key_released_ = false; }
    [[nodiscard]] bool stopped(TimePoint now) const noexcept {
        return stopped_ || (managed_ && now - heartbeat_ >= lease_timeout);
    }

    // A held key at join, mode change, mute, or reconnect must be released
    // before it can transmit. Manual mute always wins over the key state.
    [[nodiscard]] bool transmit(TimePoint now, bool key_down, bool connected) noexcept {
        if (!connected || muted_ || stopped(now) || !consent_) {
            key_released_ = false;
            return false;
        }
        if (!push_to_talk_) return true;
        if (!key_down) key_released_ = true;
        return key_down && key_released_;
    }

private:
    void command(std::string_view value, TimePoint now) {
        if (!managed_) { stop(); return; }
        if (value == "ping") heartbeat_ = now;
        else if (value == "mute") { muted_ = true; key_released_ = false; mute_requested_ = true; }
        else if (value == "unmute") muted_ = false;
        else if (value == "mode_ptt" || value == "mode_open") {
            push_to_talk_ = value == "mode_ptt";
            muted_ = true;
            mute_requested_ = true;
            key_released_ = false;
        } else if (value == "stop") stop();
        else stop(); // malformed commands never enable capture
    }

    const bool consent_;
    const bool managed_;
    bool push_to_talk_;
    bool muted_;
    bool key_released_{false};
    bool stopped_{false};
    bool mute_requested_{false};
    TimePoint heartbeat_;
    std::string pending_;
};
} // namespace gcp::transport
