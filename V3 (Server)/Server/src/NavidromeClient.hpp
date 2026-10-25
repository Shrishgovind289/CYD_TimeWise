#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

struct MusicSong {
    std::string id, title, artist, album;
    std::uint32_t duration_seconds = 0;
    nlohmann::json toJson() const {
        return {{"id",id},{"title",title},{"artist",artist},
                {"album",album},{"duration",duration_seconds}};
    }
};

class NavidromeClient {
public:
    NavidromeClient();
    MusicSong randomSong() const;
    std::string streamUrl(const std::string& id) const;
private:
    std::string base_, user_, pass_;
    std::string apiUrl(const std::string& endpoint) const;
};
