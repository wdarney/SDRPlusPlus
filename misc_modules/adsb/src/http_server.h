#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace adsb {
struct Response { int status = 200; std::string type = "application/json", body; };
class HttpServer {
public:
    using Handler = std::function<Response(const std::string&)>;
    ~HttpServer() { stop(); }
    bool start(const std::string& assets, Handler handler, std::string& error);
    void stop();
    unsigned port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
private:
    void run();
    Response route(const std::string& path);
    int socket_ = -1;
    unsigned port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::string assets_;
    Handler handler_;
};
}
