#ifndef TIMEWISE_HTTP_SERVER_HPP
#define TIMEWISE_HTTP_SERVER_HPP

#include "WeatherStation.hpp"
#include "MusicPlayer.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

class GatewayState {
public:
    nlohmann::json status() const;
    nlohmann::json enqueue(const nlohmann::json& command);
    nlohmann::json commands();
    void updateDeviceState(const nlohmann::json& state);
private:
    mutable std::mutex mutex_;
    nlohmann::json deviceState_ = {{"type", "state"}, {"device_online", false}};
    std::deque<nlohmann::json> pending_;
    std::chrono::steady_clock::time_point lastDeviceReport_{};
    std::uint64_t nextId_ = 1;
    static constexpr std::size_t MAX_PENDING = 64;
};

class HttpServer {
public:
    HttpServer(std::uint16_t port, WeatherStation& weather, GatewayState& state,
               std::string webRoot, std::string deviceToken, MusicPlayer& player);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    void start();
    void stop();
private:
    using tcp = boost::asio::ip::tcp;
    void acceptConnection();
    std::uint16_t port_;
    WeatherStation& weather_;
    GatewayState& state_;
    std::string webRoot_;
    std::string deviceToken_;
    MusicPlayer& player_;
    boost::asio::io_context ioContext_;
    tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
};
#endif
