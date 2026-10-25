#pragma once
#include "NavidromeClient.hpp"
#include <mutex>
#include <string>
#include <vector>
#include <functional>

class MusicPlayer {
public:
    MusicPlayer();
    MusicSong select(const std::string& action);
    // Streams raw unsigned 8-bit, mono, 24000 Hz PCM to callback.
    // Callback returns false when remote peer disconnects.
    void stream(const std::string& id, const std::function<bool(const char*,size_t)>& emit);
    bool isSelected(const std::string& id) const;
private:
    NavidromeClient navidrome_;
    mutable std::mutex mutex_;
    std::vector<MusicSong> history_;
    size_t cursor_ = 0;
};
