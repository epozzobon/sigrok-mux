#!/usr/bin/env python3

import socket
import sys
import struct


def main(argv):
    if len(argv) > 1:
        server_addr = argv[1]
    else:
        server_addr = './socket'

    if len(argv) > 2:
        mask = int(argv[2], 0)
    else:
        mask = 0xffffffffffffffff

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(server_addr)
    sock.send(struct.pack("<Q", mask))

    while 1:
        rcvd = sock.recv(16)
        if len(rcvd) != 16:
            break

        time, value = struct.unpack("<dQ", rcvd)
        print("%5.10f: %016x" % (time, value))

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
