#include "edge/serial.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <termios.h>
namespace edge {
Fd open_serial(const Config& c) {
    Fd fd(::open(c.serial_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    if (!fd) throw std::runtime_error("open serial: " + std::string(std::strerror(errno)));
    termios tty{};
    if (tcgetattr(fd.get(), &tty) != 0) throw std::runtime_error("tcgetattr failed");
    cfmakeraw(&tty);
    tty.c_cflag &= static_cast<tcflag_t>(~(CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS));
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    if (c.parity != 'N') { tty.c_cflag |= PARENB; if (c.parity == 'O') tty.c_cflag |= PARODD; }
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    speed_t speed = B9600;
    switch (c.baud) {
        case 19200: speed = B19200; break;
        case 38400: speed = B38400; break;
        case 57600: speed = B57600; break;
        case 115200: speed = B115200; break;
        default: break;
    }
    if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0 || tcsetattr(fd.get(), TCSANOW, &tty) != 0)
        throw std::runtime_error("serial configuration failed");
    if (tcflush(fd.get(), TCIOFLUSH) != 0) throw std::runtime_error("serial flush failed");
    return fd;
}
}
