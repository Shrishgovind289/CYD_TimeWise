
#ifndef WEATHER_STATION_HPP
#define WEATHER_STATION_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class WeatherStation
{
public:
    WeatherStation(double latitude = 40.752500, double longitude = -74.050300);

    ~WeatherStation();

    WeatherStation(const WeatherStation&) = delete;
    WeatherStation& operator=(const WeatherStation&) = delete;

    // Start / stop background weather updates.
    void start();
    void stop();

    // Request an update before the next scheduled refresh.
    void requestUpdate();

    // Returns cached JSON, or an empty string if unavailable.
    std::string getWeatherJson() const;

    // True after at least one successful fetch.
    bool hasWeatherData() const;

    // Seconds since the last successful update.
    // Returns -1 when no weather data exists.
    std::int64_t getWeatherAgeSeconds() const;

private:
    void workerLoop();
    bool fetchWeather();

    static size_t curlWriteCallback(
        char* data,
        size_t size,
        size_t count,
        void* userData
    );

    double latitude_;
    double longitude_;

    std::atomic<bool> running_{false};

    std::thread worker_;

    mutable std::mutex dataMutex_;
    std::string cachedJson_;

    std::chrono::steady_clock::time_point lastUpdate_{};
    bool hasData_ = false;

    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    bool updateRequested_ = false;

    static constexpr int UPDATE_INTERVAL_SECONDS = 1800;
    static constexpr std::size_t MAX_RESPONSE_SIZE = 65536;
};

#endif // WEATHER_STATION_HPP
