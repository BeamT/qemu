#!/usr/bin/env python3
#
# A stand-in for wmediumd's API socket (-a), for testing the wmediumd backend
# of the Wi-Fi model without root, mac80211_hwsim or a wmediumd binary.
#
# It follows the delivery rules of wmediumd (Android external/wmediumd and
# upstream): stations come from a fixed list; a frame from an unknown station
# is dropped without TX status; a unicast frame goes to the station that has
# its receiver address (its own or one added with HWSIM_CMD_ADD_MAC_ADDR) and
# is acknowledged; a group frame goes to every other station that has
# transmitted before; the sender gets HWSIM_CMD_TX_INFO_FRAME with the
# cookie. Every message pushed to a client must be answered with
# WMEDIUMD_MSG_ACK, and an ACK nobody waits for is reported as an error
# (it would abort wmediumd).
#
#   fake_wmediumd.py --api /tmp/api.sock --station 02:00:00:00:00:00 \
#       --station 02:00:00:00:02:00 --pcap air.pcap
#
# An --api of @name is an abstract socket (QEMU: path=name,abstract=on).
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import selectors
import socket
import struct
import sys
import time

MSG_INVALID, MSG_ACK, MSG_REGISTER, MSG_UNREGISTER, MSG_NETLINK, \
    MSG_SET_CONTROL, MSG_TX_START = range(7)
CMD_FRAME, CMD_TX_INFO_FRAME, CMD_ADD_MAC_ADDR, CMD_DEL_MAC_ADDR = 2, 3, 7, 8
A_RECEIVER, A_TRANSMITTER, A_FRAME, A_FLAGS, A_RX_RATE, A_SIGNAL, \
    A_TX_INFO, A_COOKIE = range(1, 9)
A_FREQ = 19
TX_STAT_ACK = 4
SIGNAL = -61                            # SNR 30 above the noise floor, -91


def mac(b):
    return ':'.join(f'{x:02x}' for x in b)


def parse_mac(s):
    return bytes(int(x, 16) for x in s.split(':'))


def nl_build(cmd, attrs):
    body = b''
    for t, v in attrs:
        body += struct.pack('=HH', 4 + len(v), t) + v
        body += b'\0' * (-len(v) % 4)
    gen = bytes([cmd, 1, 0, 0])
    return struct.pack('=IHHII', 16 + 4 + len(body), 0x20, 1, 0, 0) + gen + \
        body


def nl_parse(d):
    if len(d) < 20:
        return None, {}
    n = min(struct.unpack_from('=I', d)[0], len(d))
    cmd, attrs, off = d[16], {}, 20
    while off + 4 <= n:
        ln, t = struct.unpack_from('=HH', d, off)
        if ln < 4 or off + ln > n:
            break
        attrs[t & 0x3fff] = d[off + 4:off + ln]
        off += (ln + 3) & ~3
    return cmd, attrs


class Station:
    def __init__(self, addr):
        self.addr = addr
        self.hwaddr = addr
        self.addrs = []
        self.client = None

    def has(self, a):
        return a == self.addr or a in self.addrs


class Client:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b''
        self.registered = False
        self.wait_ack = 0


class Medium:
    def __init__(self, args):
        self.stations = [Station(parse_mac(s)) for s in args.station]
        self.sel = selectors.DefaultSelector()
        self.errors = 0
        self.frames = 0
        self.pcap = open(args.pcap, 'wb') if args.pcap else None
        if self.pcap:
            # LINKTYPE_IEEE802_11
            self.pcap.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0,
                                        65535, 105))
        path = args.api
        if path.startswith('@'):
            path = '\0' + path[1:]         # abstract socket
        elif os.path.exists(path):
            os.unlink(path)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        self.sel.register(self.srv, selectors.EVENT_READ, None)
        self.log = args.verbose

    def error(self, msg):
        self.errors += 1
        print(f'fake_wmediumd: ERROR {msg}', flush=True)

    def note(self, msg):
        if self.log:
            print(f'fake_wmediumd: {msg}', flush=True)

    def send(self, c, mtype, data=b''):
        try:
            c.sock.sendall(struct.pack('=II', mtype, len(data)) + data)
        except OSError:
            pass

    def push(self, c, nl):
        c.wait_ack += 1
        self.send(c, MSG_NETLINK, nl)

    def write_pcap(self, frame):
        if self.pcap:
            t = time.time()
            self.pcap.write(struct.pack('<IIII', int(t), int(t % 1 * 1e6),
                                        len(frame), len(frame)) + frame)
            self.pcap.flush()

    def frame(self, c, a):
        tx = a.get(A_TRANSMITTER)
        data = a.get(A_FRAME)
        for need in (A_FRAME, A_FLAGS, A_TX_INFO, A_COOKIE):
            if need not in a:
                self.error(f'HWSIM_CMD_FRAME without attribute {need}, '
                           'wmediumd would crash')
                return
        if tx is None or len(data) < 16:
            return
        sender = next((s for s in self.stations if s.addr == tx), None) or \
            next((s for s in self.stations if s.has(data[10:16])), None)
        if not sender:
            self.note(f'unknown sender {mac(tx)}, dropped')
            return
        sender.hwaddr = tx
        if sender.client is None:
            sender.client = c
        flags = struct.unpack('=I', a[A_FLAGS])[0]
        cookie = struct.unpack('=Q', a[A_COOKIE])[0]
        freq = struct.unpack('=I', a[A_FREQ])[0] if A_FREQ in a else 2412
        dest = data[4:10]
        group = dest[0] & 1
        self.frames += 1
        self.write_pcap(data)
        self.note(f'frame {mac(tx)} -> {mac(dest)} fc {data[0]:02x}'
                  f'{data[1]:02x} len {len(data)} freq {freq}')
        if group or any(s.has(dest) for s in self.stations
                        if s is not sender):
            flags |= TX_STAT_ACK
        if flags & TX_STAT_ACK:
            for st in self.stations:
                if st.addr == sender.addr:
                    continue
                if group and st.client is None:
                    continue
                if not group and not st.has(dest):
                    continue
                nl = nl_build(CMD_FRAME, [
                    (A_RECEIVER, st.hwaddr), (A_FRAME, data),
                    (A_RX_RATE, struct.pack('=I', 7)),
                    (A_FREQ, struct.pack('=I', freq)),
                    (A_SIGNAL, struct.pack('=i', SIGNAL))])
                for cl in self.clients():
                    if st.client is None or st.client is cl:
                        self.push(cl, nl)
        if sender.client:
            self.push(sender.client, nl_build(CMD_TX_INFO_FRAME, [
                (A_TRANSMITTER, sender.hwaddr),
                (A_FLAGS, struct.pack('=I', flags)),
                (A_SIGNAL, struct.pack('=i', SIGNAL)),
                (A_TX_INFO, a[A_TX_INFO]),
                (A_COOKIE, struct.pack('=Q', cookie))]))

    def mac_addr(self, cmd, a):
        st = next((s for s in self.stations
                   if s.addr == a.get(A_TRANSMITTER)), None)
        if not st or A_RECEIVER not in a:
            self.note('ADD/DEL_MAC_ADDR for an unknown station, ignored')
            return
        r = a[A_RECEIVER]
        self.note(f'{"add" if cmd == CMD_ADD_MAC_ADDR else "del"} '
                  f'{mac(r)} on {mac(st.addr)}')
        if cmd == CMD_ADD_MAC_ADDR:
            st.addrs.append(r)
        elif r in st.addrs:
            st.addrs.remove(r)

    def clients(self):
        return [k.data for k in self.sel.get_map().values()
                if k.data is not None and k.data.registered]

    def message(self, c, mtype, data):
        resp = MSG_ACK
        if mtype == MSG_ACK:
            if not c.wait_ack:
                self.error('unsolicited ACK, wmediumd would abort')
            else:
                c.wait_ack -= 1
            return
        if mtype == MSG_REGISTER:
            resp = MSG_INVALID if c.registered else MSG_ACK
            c.registered = True
        elif mtype == MSG_UNREGISTER:
            resp = MSG_ACK if c.registered else MSG_INVALID
            c.registered = False
        elif mtype == MSG_SET_CONTROL:
            pass
        elif mtype == MSG_NETLINK:
            cmd, a = nl_parse(data)
            if cmd is None:
                resp = MSG_INVALID
            self.send(c, resp)
            if cmd == CMD_FRAME:
                self.frame(c, a)
            elif cmd in (CMD_ADD_MAC_ADDR, CMD_DEL_MAC_ADDR):
                self.mac_addr(cmd, a)
            return
        else:
            resp = MSG_INVALID
        self.send(c, resp)

    def drop(self, c):
        self.sel.unregister(c.sock)
        c.sock.close()
        for st in self.stations:
            if st.client is c:
                st.client = None
        self.note('client gone')

    def run(self):
        while True:
            for key, _ in self.sel.select():
                if key.data is None:
                    s, _ = self.srv.accept()
                    self.sel.register(s, selectors.EVENT_READ, Client(s))
                    self.note('client connected')
                    continue
                c = key.data
                d = c.sock.recv(65536)
                if not d:
                    self.drop(c)
                    continue
                c.buf += d
                while len(c.buf) >= 8:
                    t, n = struct.unpack_from('=II', c.buf)
                    if len(c.buf) < 8 + n:
                        break
                    body, c.buf = c.buf[8:8 + n], c.buf[8 + n:]
                    self.message(c, t, body)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--api', required=True)
    p.add_argument('--station', action='append', required=True)
    p.add_argument('--pcap')
    p.add_argument('-v', '--verbose', action='store_true')
    args = p.parse_args()
    m = Medium(args)
    try:
        m.run()
    except KeyboardInterrupt:
        pass
    finally:
        print(f'fake_wmediumd: {m.frames} frames, {m.errors} errors',
              flush=True)


if __name__ == '__main__':
    sys.exit(main())
