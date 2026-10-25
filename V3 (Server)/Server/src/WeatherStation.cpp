
#include "WeatherStation.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

using json = nlohmann::json;

namespace
{
    constexpr const char* TAG = "WeatherStation";

    std::once_flag curlInitFlag;

    // Read a required number, rejecting missing/null values.
    double requiredNumber(
        const json& object,
        const char* key)
    {
        if (!object.is_object() || !object.contains(key) || !object.at(key).is_number())
        {
            throw std::runtime_error(std::string("Missing weather field: ") + key);
        }

        return object.at(key).get<double>();
    }

    // Read an Open-Meteo date/time string.
    std::string requiredString(const json& value)
    {
        if (!value.is_string())
        {
            throw std::runtime_error(
                "Invalid astronomy timestamp"
            );
        }

        return value.get<std::string>();
    }
}

/* =========================================================
   Constructor / Destructor
   ========================================================= */

WeatherStation::WeatherStation(double latitude, double longitude) : latitude_(latitude), longitude_(longitude)
{
    if (latitude_ < -90.0 || latitude_ > 90.0 ||longitude_ < -180.0 || longitude_ > 180.0)
    {
        throw std::invalid_argument(
            "Invalid weather coordinates"
        );
    }

    std::call_once(curlInitFlag, []()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error(
                "libcurl initialization failed"
            );
        }
    });

    std::cout
        << TAG << ": Coordinates: "
        << latitude_ << ", "
        << longitude_ << '\n';
}

WeatherStation::~WeatherStation()
{
    stop();
}

/* =========================================================
   Start background weather worker
   ========================================================= */

void WeatherStation::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    try
    {
        worker_ = std::thread(
            &WeatherStation::workerLoop,
            this
        );
    }
    catch (...)
    {
        running_ = false;
        throw;
    }

    std::cout
        << TAG << ": Weather worker started\n";
}

/* =========================================================
   Stop background weather worker
   ========================================================= */

void WeatherStation::stop()
{
    running_ = false;

    workerCv_.notify_all();

    if (worker_.joinable())
    {
        worker_.join();
    }
}

/* =========================================================
   Request an immediate weather refresh
   ========================================================= */

void WeatherStation::requestUpdate()
{
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        updateRequested_ = true;
    }

    workerCv_.notify_one();
}

/* =========================================================
   libcurl response callback
   ========================================================= */

size_t WeatherStation::curlWriteCallback(
    char* data,
    size_t size,
    size_t count,
    void* userData)
{
    if (data == nullptr || userData == nullptr)
    {
        return 0;
    }

    if (size != 0 &&
        count > std::numeric_limits<size_t>::max() / size)
    {
        return 0;
    }

    const size_t bytes = size * count;

    auto* response =
        static_cast<std::string*>(userData);

    if (bytes > MAX_RESPONSE_SIZE || response->size() > MAX_RESPONSE_SIZE - bytes)
    {
        return 0;
    }

    response->append(data, bytes);

    return bytes;
}

/* =========================================================
   Fetch weather and sunrise/sunset from Open-Meteo
   ========================================================= */

bool WeatherStation::fetchWeather()
{
    CURL* curl = curl_easy_init();

    if (curl == nullptr)
    {
        std::cerr << TAG << ": Failed to initialize HTTP client\n";

        return false;
    }

    char url[512];

    const int length = std::snprintf(
        url,
        sizeof(url),

        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.6f"
        "&longitude=%.6f"
        "&current=temperature_2m,weather_code"
        "&daily=sunrise,sunset"
        "&hourly=temperature_2m,weather_code,is_day"
        "&past_days=1"
        "&forecast_days=2"
        "&timezone=America%%2FNew_York",

        latitude_,
        longitude_
    );

    if (length < 0 || static_cast<size_t>(length) >= sizeof(url))
    {
        curl_easy_cleanup(curl);
        return false;
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        &WeatherStation::curlWriteCallback
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "TimeWise-Gateway/1.0"
    );

    std::cout
        << TAG << ": Requesting Open-Meteo weather...\n";

    const CURLcode result = curl_easy_perform(curl);

    long httpStatus = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpStatus
    );

    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
    {
        std::cerr << TAG << ": HTTPS error: " << curl_easy_strerror(result)
            << '\n';

        return false;
    }

    if (httpStatus != 200)
    {
        std::cerr
            << TAG << ": HTTP status "
            << httpStatus << '\n';

        return false;
    }

    std::cout
        << TAG << ": HTTP 200, received "
        << response.size() << " bytes\n";

    /* -----------------------------------------------------
       Parse Open-Meteo JSON
       ----------------------------------------------------- */

    try
    {
        const json source = json::parse(response);

        if (!source.contains("current") ||
            !source.at("current").is_object() ||
            !source.contains("daily") ||
            !source.at("daily").is_object())
        {
            throw std::runtime_error(
                "Missing current or daily object"
            );
        }

        const json& current = source.at("current");
        const json& daily = source.at("daily");

        /* -------------------------------------------------
           Current weather
           ------------------------------------------------- */

        const double temperature = requiredNumber(current, "temperature_2m");

        const int weatherCode = static_cast<int>(requiredNumber(current, "weather_code"));

        /* -------------------------------------------------
           Sunrise / Sunset
           ------------------------------------------------- */

        if (!daily.contains("time") || !daily.contains("sunrise") || !daily.contains("sunset"))
        {
            throw std::runtime_error(
                "Missing daily astronomy fields"
            );
        }

        const json& dates = daily.at("time");
        const json& sunrises = daily.at("sunrise");
        const json& sunsets = daily.at("sunset");

        if (!dates.is_array() || !sunrises.is_array() || !sunsets.is_array())
        {
            throw std::runtime_error("Invalid daily astronomy arrays");
        }

        const size_t count = std::min({
            dates.size(),
            sunrises.size(),
            sunsets.size()
        });

        if (count < 3)
        {
            throw std::runtime_error(
                "Expected previous, current and next day"
            );
        }

        json output;

        output["type"] = "weather";
        output["temperature"] = temperature;
        output["weather_code"] = weatherCode;
        output["daily"] = json::array();

        // No current-time calculations on the server.
        // ESP32 NTPClock will select the relevant dates.
        for (size_t i = 0; i < count; ++i)
        {
            json day;

            day["date"] = requiredString(dates.at(i));
            day["sunrise"] = requiredString(sunrises.at(i));
            day["sunset"] = requiredString(sunsets.at(i));

            output["daily"].push_back(std::move(day));
        }

	/* -------------------------------------------------
	   Hourly forecast
	   ------------------------------------------------- */

	output["hourly"] = json::array();

	if (source.contains("hourly") &&
	    source.at("hourly").is_object())
	{
	    const json& hourly = source.at("hourly");

	    if (hourly.contains("time") &&
	        hourly.contains("temperature_2m") &&
	        hourly.contains("weather_code") &&
	        hourly.contains("is_day"))
	    {
	        const json& times = hourly.at("time");
	        const json& temperatures = hourly.at("temperature_2m");
	        const json& codes = hourly.at("weather_code");
	        const json& day_flags = hourly.at("is_day");

	        if (times.is_array() &&
	            temperatures.is_array() &&
	            codes.is_array() &&
	            day_flags.is_array())
	        {
	            const size_t hourly_count = std::min({
	                times.size(),
	                temperatures.size(),
	                codes.size(),
	                day_flags.size()
	            });

	            for (size_t i = 0; i < hourly_count; ++i)
	            {
	                if (!times.at(i).is_string() ||
	                    !temperatures.at(i).is_number() ||
	                    !codes.at(i).is_number() ||
	                    !day_flags.at(i).is_number())
	                {
	                    continue;
	                }

	                json hour;

	                hour["time"] = times.at(i);
	                hour["temperature_2m"] = temperatures.at(i);
	                hour["weather_code"] = codes.at(i);
	                hour["is_day"] = day_flags.at(i);

	                output["hourly"].push_back(std::move(hour));
	            }
	        }
	    }
	}


        /* -------------------------------------------------
           Update cached JSON
           ------------------------------------------------- */

        const std::string compactJson = output.dump();

        {
            std::lock_guard<std::mutex> lock(dataMutex_);

            cachedJson_ = compactJson;
            lastUpdate_ = std::chrono::steady_clock::now();
            hasData_ = true;
        }

        /* -------------------------------------------------
           Print output to console
           ------------------------------------------------- */

        std::cout
            << "========================================\n"
            << "          TIMEWISE WEATHER\n"
            << "Temperature  : " << temperature << " C\n"
            << "Weather Code : " << weatherCode << '\n'
            << "\n"
            << "Sunrise / Sunset:\n"
            << "----------------------------------------\n";

        for (const auto& day : output["daily"])
        {
            std::cout
                << "Date    : " << day.at("date").get<std::string>()
                << '\n'
                << "Sunrise : " << day.at("sunrise").get<std::string>()
                << '\n'
                << "Sunset  : " << day.at("sunset").get<std::string>()
                << "\n";
        }

        std::cout << "========================================\n" << TAG << ": Weather cache updated\n\n";

        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr<< TAG << ": Parsing failed: " << error.what() << '\n';

        return false;
    }
}

/* =========================================================
   Background weather scheduler
   ========================================================= */

void WeatherStation::workerLoop()
{
    while (running_)
    {
        const bool success = fetchWeather();

        const auto delay = std::chrono::seconds(success ? UPDATE_INTERVAL_SECONDS : 60);

        std::unique_lock<std::mutex> lock(workerMutex_);

        workerCv_.wait_for(
            lock,
            delay,
            [this]()
            {
                return !running_ || updateRequested_;
            }
        );

        updateRequested_ = false;
    }
}

/* =========================================================
   Cached weather access
   ========================================================= */

std::string WeatherStation::getWeatherJson() const
{
    std::lock_guard<std::mutex> lock(dataMutex_);

    return cachedJson_;
}

bool WeatherStation::hasWeatherData() const
{
    std::lock_guard<std::mutex> lock(dataMutex_);

    return hasData_;
}

std::int64_t WeatherStation::getWeatherAgeSeconds() const
{
    std::lock_guard<std::mutex> lock(dataMutex_);

    if (!hasData_)
    {
        return -1;
    }

    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastUpdate_).count();
}
