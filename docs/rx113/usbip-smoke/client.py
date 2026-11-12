#!/usr/bin/env python3
"""Independent USB/IP client written directly from
Documentation/usb/usbip_protocol.rst.  Drives the RX113 model's
function adapter: device list, import, enumeration (standard
descriptors + SET_CONFIGURATION), then a bulk echo round trip
against the firmware in fw.c (EP2 OUT -> EP1 IN).
"""
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 3240
DEVID = (1 << 16) | 1


def recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"closed after {len(buf)}/{n} bytes")
        buf += chunk
    return buf


class Client:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=30)
        self.seq = 0

    def op(self, code, payload=b""):
        self.sock.sendall(struct.pack(">HHI", 0x0111, code, 0) + payload)
        hdr = recvn(self.sock, 8)
        ver, rcode, status = struct.unpack(">HHI", hdr)
        assert ver == 0x0111, hex(ver)
        assert rcode == code & 0x7FFF, (hex(rcode), hex(code))
        return status

    def devlist(self):
        status = self.op(0x8005)
        assert status == 0
        (count,) = struct.unpack(">I", recvn(self.sock, 4))
        assert count == 1, count
        rec = recvn(self.sock, 312)
        busid = rec[256:288].split(b"\0")[0].decode()
        busnum, devnum, speed = struct.unpack(">III", rec[288:300])
        assert busid == "1-1", busid
        print(f"DEVLIST ok: busid={busid} bus={busnum} dev={devnum} "
              f"speed={speed}")
        return busid

    def import_(self, busid):
        status = self.op(0x8003, busid.encode().ljust(32, b"\0"))
        assert status == 0
        rec = recvn(self.sock, 312)  # full record, path included
        got = rec[256:288].split(b"\0")[0].decode()
        assert got == busid, got
        print("IMPORT ok (full 312-byte record received)")

    def submit(self, ep, direction, data=b"", want=0, setup=None,
               timeout=20):
        self.seq += 1
        seq = self.seq
        flags = 0
        length = want if direction == 1 else len(data)
        hdr = struct.pack(
            ">IIIII iiiii 8s",
            1, seq, DEVID, direction, ep,
            flags, length, 0, -1, 0,
            setup if setup is not None else b"\0" * 8)
        assert len(hdr) == 48
        self.sock.sendall(hdr + (data if direction == 0 else b""))
        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                raise TimeoutError(f"URB seq={seq} ep={ep}")
            ret = recvn(self.sock, 48)
            cmd, rseq, _dev, _dir, _ep = struct.unpack(">IIIII", ret[:20])
            assert cmd == 3, cmd  # RET_SUBMIT
            status, actual = struct.unpack(">ii", ret[20:28])
            payload = recvn(self.sock, actual) if actual > 0 else b""
            if rseq == seq:
                return status, payload
            print(f"  (stray RET seq={rseq}, ignored)")

    def control(self, setup, want=0):
        direction = 1 if setup[0] & 0x80 else 0
        data = b""
        if direction == 0 and len(setup) == 8:
            pass
        status, payload = self.submit(0, direction, data, want, setup)
        return status, payload


def main():
    c = Client()
    busid = c.devlist()
    c.import_(busid)
    time.sleep(0.5)

    # GET_DESCRIPTOR (device, 18 bytes)
    st, data = c.control(bytes([0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0]),
                         want=18)
    assert st == 0 and len(data) == 18, (st, len(data))
    vid, pid = struct.unpack("<HH", data[8:12])
    print(f"GET_DESCRIPTOR device ok: VID={vid:04x} PID={pid:04x}")
    assert (vid, pid) == (0x1209, 0x7411)

    # GET_DESCRIPTOR (configuration, full 32 bytes)
    st, data = c.control(bytes([0x80, 0x06, 0x00, 0x02, 0, 0, 32, 0]),
                         want=32)
    assert st == 0 and len(data) == 32, (st, len(data))
    total, nep = struct.unpack("<HB", data[2:5])[0], data[22]
    print(f"GET_DESCRIPTOR config ok: total={total}")

    # SET_CONFIGURATION 1 (no data stage)
    st, _ = c.control(bytes([0x00, 0x09, 0x01, 0x00, 0, 0, 0, 0]))
    assert st == 0, st
    print("SET_CONFIGURATION ok")
    time.sleep(0.5)

    # Bulk echo round trips: 100 bytes (two packets), 5, and 64.
    for size in (100, 5, 64):
        payload = bytes((i * 7 + size) & 0xFF for i in range(size))
        st, _ = c.submit(2, 0, payload)
        assert st == 0, st
        st, back = c.submit(1, 1, want=size)
        assert st == 0, st
        assert back == payload, f"echo mismatch at {size}: {len(back)}"
        print(f"bulk echo {size} bytes ok")

    print("LIVE SMOKE: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
