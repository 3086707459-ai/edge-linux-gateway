#include "edge/http.hpp"
#include "edge/model.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <vector>
namespace edge {
namespace {
constexpr std::size_t max_headers = 4096;
constexpr std::size_t max_clients = 32;
std::string trim_header(const std::string& s) {
    const auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    return s.substr(start, s.find_last_not_of(" \t") - start + 1);
}
bool same_secret(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
const char* reason(int status) {
    switch (status) {
        case 200: return "OK"; case 400: return "Bad Request";
        case 401: return "Unauthorized"; case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable"; default: return "Internal Server Error";
    }
}
Response error_response(int status) {
    return {status, "application/json", "{\"error\":\"" + std::string(reason(status)) + "\"}"};
}
void watch(int epoll, int fd, std::uint32_t events, int op) {
    epoll_event ev{}; ev.events = events; ev.data.fd = fd;
    if (epoll_ctl(epoll, op, fd, &ev) != 0) throw std::runtime_error("HTTP epoll_ctl failed");
}
}
Request parse_request(const std::string& raw) {
    Request r;
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos || end + 4 != raw.size()) { r.error = 400; return r; }
    if (raw.size() > max_headers) { r.error = 431; return r; }
    for (const unsigned char ch : raw) if ((ch < 32 && ch != '\r' && ch != '\n' && ch != '\t') || ch == 127) { r.error = 400; return r; }
    const auto first = raw.find("\r\n");
    std::istringstream line(raw.substr(0, first));
    std::string method, version, extra;
    if (!(line >> method >> r.target >> version) || (line >> extra) ||
        (version != "HTTP/1.1" && version != "HTTP/1.0") || r.target.empty() || r.target[0] != '/' || r.target.size() > 1024) {
        r.error = 400; return r;
    }
    if (method != "GET") { r.error = 405; return r; }
    std::set<std::string> names;
    auto pos = first + 2;
    bool host = false;
    while (pos < end) {
        const auto stop = raw.find("\r\n", pos);
        const auto header = raw.substr(pos, stop - pos);
        const auto colon = header.find(':');
        if (colon == std::string::npos || colon == 0) { r.error = 400; return r; }
        auto name = header.substr(0, colon);
        for (char& ch : name) {
            const auto u = static_cast<unsigned char>(ch);
            if (!(std::isalnum(u) || ch == '-')) { r.error = 400; return r; }
            ch = static_cast<char>(std::tolower(u));
        }
        const auto value = trim_header(header.substr(colon + 1));
        if (value.find_first_of("\r\n") != std::string::npos || !names.insert(name).second) { r.error = 400; return r; }
        if (name == "host") host = !value.empty();
        if (name == "authorization") r.authorization = value;
        if (name == "transfer-encoding" || (name == "content-length" && value != "0")) { r.error = 400; return r; }
        pos = stop + 2;
    }
    if (version == "HTTP/1.1" && !host) r.error = 400;
    return r;
}
HttpServer::HttpServer(int epoll, const std::string& bind, int port, std::string token,
                       std::function<Response(const std::string&)> handler)
    : epoll_(epoll), listener_(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
      token_(std::move(token)), handler_(std::move(handler)) {
    if (!listener_) throw std::runtime_error("socket failed");
    int yes = 1;
    if (setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) throw std::runtime_error("setsockopt failed");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1 ||
        ::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener_.get(), 32) != 0)
        throw std::runtime_error("HTTP bind/listen failed; check address and port");
    watch(epoll_, listener_.get(), EPOLLIN, EPOLL_CTL_ADD);
}
void HttpServer::close_client(int fd) {
    epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
    clients_.erase(fd);
}
void HttpServer::reply(Client& client, Response response) {
    client.output = "HTTP/1.1 " + std::to_string(response.status) + ' ' + reason(response.status) + "\r\nContent-Type: " + response.type +
        "; charset=utf-8\r\nContent-Length: " + std::to_string(response.body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'; object-src 'none'\r\n";
    if (response.status == 401) client.output += "WWW-Authenticate: Bearer\r\n";
    if (response.status == 405) client.output += "Allow: GET\r\n";
    client.output += "\r\n" + response.body;
    client.input.clear();
    client.deadline = monotonic_ms() + 2000;
    watch(epoll_, client.fd.get(), EPOLLOUT | EPOLLRDHUP, EPOLL_CTL_MOD);
}
bool HttpServer::handle(int fd, std::uint32_t events) {
    if (fd == listener_.get()) {
        for (int i = 0; i < 16; ++i) {
            Fd socket(accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!socket) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                throw std::runtime_error("accept failed");
            }
            if (clients_.size() >= max_clients) continue;
            const int n = socket.get();
            watch(epoll_, n, EPOLLIN | EPOLLRDHUP, EPOLL_CTL_ADD);
            clients_.emplace(n, Client{std::move(socket), {}, {}, 0, monotonic_ms() + 2000});
        }
        return true;
    }
    auto it = clients_.find(fd);
    if (it == clients_.end()) return false;
    auto& client = it->second;
    if ((events & EPOLLERR) != 0) { close_client(fd); return true; }
    if ((events & EPOLLIN) != 0 && client.output.empty()) {
        char buffer[1024];
        while (client.output.empty()) {
            const auto n = recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                client.input.append(buffer, static_cast<std::size_t>(n));
                if (client.input.size() > max_headers) { reply(client, error_response(431)); break; }
                if (client.input.find("\r\n\r\n") != std::string::npos) {
                    const auto req = parse_request(client.input);
                    if (req.error) reply(client, error_response(req.error));
                    else if (!token_.empty() && req.target != "/" && !same_secret(req.authorization, "Bearer " + token_))
                        reply(client, error_response(401));
                    else {
                        try { reply(client, handler_(req.target)); }
                        catch (const std::exception&) { reply(client, error_response(503)); }
                    }
                    break;
                }
            } else if (n == 0) { close_client(fd); return true; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { close_client(fd); return true; }
        }
    }
    if ((events & EPOLLOUT) != 0 && !client.output.empty()) {
        while (client.sent < client.output.size()) {
            const auto n = send(fd, client.output.data() + client.sent, client.output.size() - client.sent, MSG_NOSIGNAL);
            if (n > 0) client.sent += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else { close_client(fd); return true; }
        }
        if (client.sent == client.output.size()) { close_client(fd); return true; }
    }
    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0 && client.output.empty()) close_client(fd);
    return true;
}
void HttpServer::expire(std::int64_t now) {
    std::vector<int> expired;
    for (const auto& item : clients_) if (item.second.deadline <= now) expired.push_back(item.first);
    for (const int fd : expired) close_client(fd);
}
}
