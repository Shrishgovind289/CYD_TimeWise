#include "HttpServer.hpp"
#include "WeatherStation.hpp"
#include "MusicPlayer.hpp"
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>
#include <chrono>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void signalHandler(int) { stopRequested = 1; }
}
int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    try {
        constexpr std::uint16_t HTTP_PORT = 8766;
        constexpr double LATITUDE = 40.752500;
        constexpr double LONGITUDE = -74.050300;
        const char* root = std::getenv("TIMEWISE_WEB_ROOT");
        const char* token = std::getenv("TIMEWISE_DEVICE_TOKEN");
        if (!token || !*token) {
            std::cerr << "Set TIMEWISE_DEVICE_TOKEN to a long random secret before starting.\n";
            return 1;
        }
        WeatherStation weather(LATITUDE, LONGITUDE);
        GatewayState state;
        MusicPlayer musicPlayer;
        HttpServer server(HTTP_PORT, weather, state, root ? root : "./web", token, musicPlayer);
        weather.start();
        server.start();
        std::cout << "TimeWise HTTP Gateway: port " << HTTP_PORT << '\n';
        while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.stop();
        weather.stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TimeWise failure: " << e.what() << '\n';
        return 1;
    }
}
