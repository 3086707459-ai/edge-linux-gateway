#pragma once
#include "edge/fd.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
namespace edge {
struct Response { int status = 200; std::string type = "application/json"; std::string body; };
struct Request { std::string target; std::string authorization; int error = 0; };
Request parse_request(const std::string& raw);
class HttpServer {
    struct Client {
        Fd fd;
        std::string input, output;
        std::size_t sent = 0;
        std::int64_t deadline = 0;
    };
    int epoll_;
    Fd listener_;
    std::unordered_map<int, Client> clients_;
    std::string token_;
    std::function<Response(const std::string&)> handler_;
    void reply(Client& client, Response response);
    void close_client(int fd);
public:
    HttpServer(int epoll, const std::string& bind, int port, std::string token,
               std::function<Response(const std::string&)> handler);
    bool handle(int fd, std::uint32_t events);
    void expire(std::int64_t now);
};
}
