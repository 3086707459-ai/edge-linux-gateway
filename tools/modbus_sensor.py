#!/usr/bin/env python3
"""Standard-library Modbus RTU sensor on a Linux PTY, with fault injection."""
import argparse
import errno
import math
import os
import pty
import select
import struct
import time
import tty


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def frame(data):
    return data + struct.pack('<H', crc16(data))


class Sensor:
    def __init__(self, link, unit=1, bad_crc_every=0, drop_every=0,
                 fragment=False, exception_every=0, temperature=None):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.link = link
        self.unit = unit
        self.bad_crc_every = bad_crc_every
        self.drop_every = drop_every
        self.exception_every = exception_every
        self.fragment = fragment
        self.temperature = temperature
        self.requests = 0
        self.buffer = bytearray()
        # Refuse to overwrite a real path. Existing symlinks are owned by the caller.
        if os.path.lexists(link):
            if not os.path.islink(link):
                self.close()
                raise ValueError('link already exists and is not a symlink')
            os.unlink(link)
        os.symlink(os.ttyname(self.slave), link)

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass
        if os.path.islink(self.link):
            os.unlink(self.link)

    def poll(self, timeout=0.02):
        if not select.select([self.master], [], [], timeout)[0]:
            return
        try:
            data = os.read(self.master, 256)
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EIO):
                return
            raise
        self.buffer.extend(data)
        while len(self.buffer) >= 8:
            request = bytes(self.buffer[:8])
            if crc16(request[:6]) != struct.unpack('<H', request[6:])[0]:
                del self.buffer[0]
                continue
            del self.buffer[:8]
            if request[0] != self.unit:
                continue
            self.requests += 1
            if self.drop_every and self.requests % self.drop_every == 0:
                continue
            if (request[1] != 4 or request[2:6] != b'\x00\x00\x00\x02' or
                    (self.exception_every and self.requests % self.exception_every == 0)):
                response = frame(bytes([self.unit, request[1] | 0x80, 2]))
            else:
                phase = self.requests % 20
                temp = self.temperature if self.temperature is not None else 24 + min(phase, 20 - phase) * 0.9
                humidity = 50 + 8 * math.sin(phase)
                response = frame(bytes([self.unit, 4, 4]) + struct.pack('>hH', round(temp * 10), round(humidity * 10)))
            if self.bad_crc_every and self.requests % self.bad_crc_every == 0:
                response = response[:-1] + bytes([response[-1] ^ 1])
            if self.fragment:
                os.write(self.master, response[:3])
                time.sleep(0.005)
                os.write(self.master, response[3:])
            else:
                os.write(self.master, response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--link', default='/tmp/edge-sensor')
    parser.add_argument('--unit', type=int, default=1)
    parser.add_argument('--bad-crc-every', type=int, default=0)
    parser.add_argument('--drop-every', type=int, default=0)
    parser.add_argument('--exception-every', type=int, default=0)
    parser.add_argument('--fragment', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.unit <= 247 or min(args.bad_crc_every, args.drop_every, args.exception_every) < 0:
        parser.error('unit must be 1..247; fault intervals must be nonnegative')
    sensor = Sensor(args.link, args.unit, args.bad_crc_every, args.drop_every,
                    args.fragment, args.exception_every)
    print(f'Sensor ready: {args.link} -> {os.ttyname(sensor.slave)}', flush=True)
    try:
        while True:
            sensor.poll()
    except KeyboardInterrupt:
        pass
    finally:
        sensor.close()


if __name__ == '__main__':
    main()
