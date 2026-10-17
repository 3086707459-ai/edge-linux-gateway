#pragma once
#include <unistd.h>
#include <utility>
namespace edge {
class Fd {
    int fd_ = -1;
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(std::exchange(other.fd_, -1));
        return *this;
    }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset(int fd = -1) noexcept {
        // On Linux, retrying close after EINTR may close an unrelated reused fd.
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
};
}
