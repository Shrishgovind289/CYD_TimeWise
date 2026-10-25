#include "HttpServer.hpp"
#include "MusicPlayer.hpp"
#include <sys/socket.h>
#include <thread>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

json GatewayState::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = deviceState_;
    result["device_online"] = lastDeviceReport_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - lastDeviceReport_ < std::chrono::seconds(15);
    return result;
}
json GatewayState::enqueue(const json& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() >= MAX_PENDING) throw std::runtime_error("command_queue_full");
    json message = command;
    message["id"] = nextId_++;
    pending_.push_back(message);
    return {{"accepted", true}, {"id", message["id"]}};
}
json GatewayState::commands() {
    std::lock_guard<std::mutex> lock(mutex_);
    json result = json::array();
    // Delivery is at-most-once: commands leave the queue when fetched.
    // Device must reconnect / retry failed actions as needed.
    while (!pending_.empty()) {
        result.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    return {{"commands", result}};
}
void GatewayState::updateDeviceState(const json& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceState_ = state;
    deviceState_["type"] = "state";
    deviceState_["device_online"] = true;
    lastDeviceReport_ = std::chrono::steady_clock::now();
}

namespace {
constexpr int TIMEOUT_SECONDS = 15;
constexpr unsigned int WORKERS = 2;

bool validCommand(const json& j) {
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) return false;
    const std::string cmd = j["cmd"].get<std::string>();
    if (cmd == "volume" || cmd == "brightness" || cmd == "music_volume") {
        return j.contains("value") && j["value"].is_number_integer() &&
               j["value"].get<int>() >= 0 && j["value"].get<int>() <= 100;
    }
    if (cmd == "alarm") {
        return j.contains("hour") && j["hour"].is_number_integer() &&
               j["hour"].get<int>() >= 0 && j["hour"].get<int>() <= 23 &&
               j.contains("minute") && j["minute"].is_number_integer() &&
               j["minute"].get<int>() >= 0 && j["minute"].get<int>() <= 59 &&
               j.contains("enabled") && j["enabled"].is_boolean();
    }
    if (cmd == "snooze") {
        return j.contains("minutes") && j["minutes"].is_number_integer() &&
               (j["minutes"] == 5 || j["minutes"] == 10 ||
                j["minutes"] == 15 || j["minutes"] == 30);
    }
    if (cmd == "music_enable") return j.contains("enabled") && j["enabled"].is_boolean();
    return cmd == "stop" || cmd == "music_play_random" ||
           cmd == "music_previous" || cmd == "music_next" ||
           cmd == "music_stop" || cmd == "music_pause" || cmd == "music_resume";
}

std::string contentType(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js") return "text/javascript; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css") return "text/css; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".png") return "image/png";
    return "application/octet-stream";
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, WeatherStation& weather, GatewayState& state,
                const std::string& root, const std::string& token, MusicPlayer& player)
        : stream_(std::move(socket)), weather_(weather), state_(state), root_(root), token_(token), player_(player) {}
    void start() { read(); }
private:
    void read() {
        parser_.emplace();
        parser_->body_limit(8192);
        stream_.expires_after(std::chrono::seconds(TIMEOUT_SECONDS));
        http::async_read(stream_, buffer_, *parser_,
            beast::bind_front_handler(&HttpSession::onRead, shared_from_this()));
    }
    void onRead(beast::error_code ec, std::size_t) {
        if (ec) { close(); return; }
        request_ = parser_->release();
        process();
    }
    void process() {
        response_ = std::make_shared<http::response<http::string_body>>();
        response_->version(request_.version());
        response_->keep_alive(false);
        response_->set(http::field::server, "TimeWiseGateway/2.0");
        response_->set(http::field::cache_control, "no-store");
        response_->set(http::field::content_type, "application/json");
        const std::string path(request_.target());
        auto reply = [this](http::status status, const json& body) {
            response_->result(status);
            response_->body() = body.dump();
        };
        try {
            if (path.rfind("/music/stream?", 0) == 0 && request_.method() == http::verb::get) {
                if (token_.empty() || request_["X-Device-Token"] != token_) {
                    reply(http::status::unauthorized, {{"error", "unauthorized"}});
                } else {
                    const std::string marker = "id=";
                    auto pos = path.find(marker, std::string("/music/stream?").size());
                    if (pos == std::string::npos) {
                        reply(http::status::bad_request, {{"error", "missing_id"}});
                    } else {
                        std::string encoded = path.substr(pos + marker.size());
                        if (encoded.find('&') != std::string::npos || encoded.size() > 512) {
                            reply(http::status::bad_request, {{"error", "invalid_id"}});
                        } else {
                            std::string id;
                            for (size_t i=0;i<encoded.size();++i) {
                                if(encoded[i]=='%' && i+2<encoded.size()) {
                                    auto hex=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; if(c>='a'&&c<='f')return c-'a'+10; return -1; };
                                    int hi=hex(encoded[i+1]),lo=hex(encoded[i+2]);
                                    if(hi<0||lo<0){id.clear();break;}
                                    id.push_back(char((hi<<4)|lo));i+=2;
                                } else id.push_back(encoded[i]);
                            }
                            if (id.empty() || !player_.isSelected(id)) {
                                reply(http::status::not_found, {{"error", "song_not_selected"}});
                            } else {
                                // Raw close-delimited HTTP/1.0 body. No WAV header or chunk framing.
                                std::thread([self=shared_from_this(), id]() {
                                    self->streamMusic(id);
                                }).detach();
                                return;
                            }
                        }
                    }
                }
            } else if (path == "/music/select" && request_.method() == http::verb::post) {
                if (token_.empty() || request_["X-Device-Token"] != token_) {
                    reply(http::status::unauthorized, {{"error", "unauthorized"}});
                } else {
                    const auto body=json::parse(request_.body());
                    if (!body.is_object() || !body.contains("action") || !body["action"].is_string())
                        reply(http::status::bad_request, {{"error","invalid_action"}});
                    else reply(http::status::ok, player_.select(body["action"].get<std::string>()).toJson());
                }
            } else if (path == "/health" && request_.method() == http::verb::get) {
                reply(http::status::ok, {{"service", "TimeWiseGateway"}, {"status", "ok"}});
            } else if ((path == "/weather" || path == "/api/weather") && request_.method() == http::verb::get) {
                const auto weatherJson = weather_.getWeatherJson();
                if (weatherJson.empty()) reply(http::status::service_unavailable, {{"error", "weather_unavailable"}});
                else {
                    json current = json::parse(weatherJson);
                    current.erase("hourly");
                    response_->result(http::status::ok);
                    response_->body() = current.dump();
                }
            } else if ((path == "/forecast" || path == "/api/forecast") && request_.method() == http::verb::get) {
                const auto weatherJson = weather_.getWeatherJson();
                if (weatherJson.empty()) {
                    reply(http::status::service_unavailable, {{"error", "weather_unavailable"}});
                } else {
                    const json cached = json::parse(weatherJson);
                    json result = {{"type", "forecast"}, {"hourly", json::array()}};
                    // ESP32 expects times local to the Gateway's TZ (America/New_York).
                    const std::time_t now = std::time(nullptr);
                    std::tm local{};
                    localtime_r(&now, &local);
                    char currentHour[20]{};
                    std::strftime(currentHour, sizeof(currentHour), "%Y-%m-%dT%H", &local);
                    if (cached.contains("hourly") && cached.at("hourly").is_array()) {
                        for (const auto& hour : cached.at("hourly")) {
                            if (!hour.is_object() || !hour.contains("time") || !hour.at("time").is_string()) continue;
                            const std::string t = hour.at("time").get<std::string>();
                            if (t.size() < 16 || t.substr(0, 13) <= currentHour) continue;
                            result["hourly"].push_back(hour);
                            if (result["hourly"].size() == 5) break;
                        }
                    }
                    response_->result(http::status::ok);
                    response_->body() = result.dump();
                }
            } else if (path == "/api/weather/update" && request_.method() == http::verb::post) {
                weather_.requestUpdate();
                reply(http::status::accepted, {{"accepted", true}});
            } else if (path == "/api/status" && request_.method() == http::verb::get) {
                reply(http::status::ok, state_.status());
            } else if (path == "/api/control" && request_.method() == http::verb::post) {
                json command = json::parse(request_.body());
                if (!validCommand(command)) reply(http::status::bad_request, {{"error", "invalid_command"}});
                else reply(http::status::accepted, state_.enqueue(command));
            } else if ((path == "/device/commands" || path == "/device/state") &&
                       (token_.empty() || request_["X-Device-Token"] != token_)) {
                reply(http::status::unauthorized, {{"error", "unauthorized"}});
            } else if (path == "/device/commands" && request_.method() == http::verb::get) {
                reply(http::status::ok, state_.commands());
            } else if (path == "/device/state" && request_.method() == http::verb::post) {
                json incoming = json::parse(request_.body());
                if (!incoming.is_object()) reply(http::status::bad_request, {{"error", "invalid_state"}});
                else { state_.updateDeviceState(incoming); reply(http::status::ok, {{"ok", true}}); }
            } else if (request_.method() == http::verb::get &&
                       (path == "/" || path == "/index.html" || path == "/script.js" ||
                        path == "/style.css" || path == "/Logo.png" || path == "/DRAW.png")) {
                const std::string file = path == "/" ? "index.html" : path.substr(1);
                std::ifstream input(root_ + "/" + file, std::ios::binary);
                if (!input) reply(http::status::not_found, {{"error", "file_not_found"}});
                else {
                    std::ostringstream bytes; bytes << input.rdbuf();
                    response_->result(http::status::ok);
                    response_->set(http::field::content_type, contentType(file));
                    response_->body() = bytes.str();
                }
            } else if (path == "/audio") {
                reply(http::status::not_implemented, {{"error", "audio_not_implemented"}});
            } else reply(http::status::not_found, {{"error", "not_found"}});
        } catch (const json::exception&) {
            reply(http::status::bad_request, {{"error", "invalid_json"}});
        } catch (const std::exception& e) {
            reply(http::status::service_unavailable, {{"error", e.what()}});
        }
        response_->prepare_payload();
        stream_.expires_after(std::chrono::seconds(TIMEOUT_SECONDS));
        http::async_write(stream_, *response_,
            beast::bind_front_handler(&HttpSession::onWrite, shared_from_this()));
    }
    void streamMusic(const std::string& id) {
        // The TCP connection belongs to this session for the stream lifetime.
        stream_.expires_never();
        const std::string header =
            "HTTP/1.0 200 OK\r\nServer: TimeWiseGateway/2.0\r\n"
            "Content-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n";
        auto writeAll=[this](const char* data,size_t size)->bool {
            while(size>0){
                ssize_t wrote=::send(stream_.socket().native_handle(),data,size,MSG_NOSIGNAL);
                if(wrote<=0) return false;
                data+=wrote;size-=static_cast<size_t>(wrote);
            }
            return true;
        };
        if(writeAll(header.data(),header.size())) {
            try { player_.stream(id,writeAll); }
            catch(const std::exception& e) { std::cerr<<"Music stream failed: "<<e.what()<<'\n'; }
        }
        close();
    }
    void onWrite(beast::error_code, std::size_t) { close(); }
    void close() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
        stream_.socket().close(ec);
    }
    beast::tcp_stream stream_;
    WeatherStation& weather_;
    GatewayState& state_;
    const std::string& root_;
    const std::string& token_;
    MusicPlayer& player_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    http::request<http::string_body> request_;
    std::shared_ptr<http::response<http::string_body>> response_;
};
} // namespace

HttpServer::HttpServer(std::uint16_t port, WeatherStation& weather, GatewayState& state,
                       std::string webRoot, std::string deviceToken, MusicPlayer& player)
    : port_(port), weather_(weather), state_(state), webRoot_(std::move(webRoot)),
      deviceToken_(std::move(deviceToken)), player_(player), acceptor_(ioContext_) {}
HttpServer::~HttpServer() { stop(); }
void HttpServer::start() {
    if (running_.exchange(true)) return;
    try {
        ioContext_.restart();
        tcp::endpoint endpoint(tcp::v4(), port_);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        acceptConnection();
        for (unsigned i = 0; i < WORKERS; ++i) workers_.emplace_back([this] { ioContext_.run(); });
        std::cout << "HTTP listening on " << port_ << '\n';
    } catch (...) { stop(); throw; }
}
void HttpServer::acceptConnection() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) std::make_shared<HttpSession>(std::move(socket), weather_, state_,
                                                webRoot_, deviceToken_, player_)->start();
        if (running_ && acceptor_.is_open()) acceptConnection();
    });
}
void HttpServer::stop() {
    if (!running_.exchange(false) && workers_.empty()) return;
    ioContext_.stop();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
    beast::error_code ec;
    acceptor_.close(ec);
}
