#!/usr/bin/env python3
#
# Wi-Fi emulation test matrix for the Espressif QEMU machines
#
# Builds ESP-IDF examples and test apps for each IDF version and chip, boots
# them in QEMU with the emulated Wi-Fi MAC, and decides pass/fail from the
# serial output and the traffic seen on the netdev.
#
#   tests/esp_wifi/run_matrix.py --idf ~/esp/esp-idf-v6.1 --chips esp32
#
# Every run keeps its serial log, pcap and unimplemented-register log in the
# work directory; a Markdown report lists the results.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import http.server
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
APPS = HERE / 'apps'
MKEFUSE = HERE / 'mkefuse.py'

CHIPS = {
    'esp32': {
        'qemu': 'qemu-system-xtensa',
        'machine': ['-machine', 'esp32', '-m', '4M'],
        'efuse': 'nvram.esp32.efuse',
        'wifi': 'misc.esp32_wifi',
    },
    'esp32c3': {
        'qemu': 'qemu-system-riscv32',
        'machine': ['-machine', 'esp32c3'],
        'efuse': 'nvram.esp32c3.efuse',
        'wifi': 'misc.esp32c3_wifi',
    },
    'esp32s3': {
        'qemu': 'qemu-system-xtensa',
        'machine': ['-machine', 'esp32s3'],
        'efuse': 'nvram.esp32s3.efuse',
        'wifi': 'misc.esp32s3_wifi',
    },
}

# Applied to every build: flash modes and sizes the QEMU flash model supports
COMMON_SDKCONFIG = '''
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y
'''

PSK = 'qemu-password'


def idf_tag(idf):
    """Version of an IDF checkout, e.g. v6.1 or v5.5.6"""
    try:
        text = (idf / 'tools' / 'cmake' / 'version.cmake').read_text()
        v = [re.search(rf'IDF_VERSION_{k} (\d+)', text).group(1)
             for k in ('MAJOR', 'MINOR', 'PATCH')]
    except (OSError, AttributeError):
        return idf.name
    return 'v' + '.'.join(v if v[2] != '0' else v[:2])


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Result:
    def __init__(self, ok, detail):
        self.ok = ok
        self.detail = detail


# QEMU process with a serial console we can wait on and type into

class Qemu:
    def __init__(self, ctx, chip, flash, name, nic, mac, extra=(), wifi=(),
                 reboot=False):
        c = CHIPS[chip]
        self.log_path = ctx.run_dir / f'{name}.log'
        self.unimp_path = ctx.run_dir / f'{name}.unimp'
        efuse = ctx.run_dir / f'{name}.efuse'
        subprocess.check_call([sys.executable, str(MKEFUSE), '--chip', chip,
                               mac, str(efuse)])
        # A private copy, so that NVS state does not leak between runs
        image = ctx.run_dir / f'{name}.flash'
        shutil.copy(flash, image)
        cmd = [str(ctx.qemu_dir / c['qemu']), '-nographic', *c['machine'],
               '-drive', f'file={image},if=mtd,format=raw',
               '-drive', f'file={efuse},if=none,format=raw,id=efuse',
               '-global', f'driver={c["efuse"]},property=drive,value=efuse',
               '-netdev', f'{nic},id=n0',
               '-net', f'nic,netdev=n0,model={c["wifi"]}',
               '-object', f'filter-dump,id=f0,netdev=n0,'
                          f'file={ctx.run_dir / name}.pcap',
               '-d', 'unimp', '-D', str(self.unimp_path)]
        if not reboot:
            # A guest reset stops QEMU instead of restarting the chip
            cmd.append('-no-reboot')
        if ctx.icount:
            cmd += ['-icount', 'shift=auto']
        for prop, value in wifi:
            cmd += ['-global', f'driver={c["wifi"]},property={prop},'
                               f'value={value}']
        cmd += list(extra)
        self.cmd = cmd
        self.lines = []
        self.cond = threading.Condition()
        self.log = open(self.log_path, 'w', errors='replace')
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for raw in self.p.stdout:
            line = raw.decode(errors='replace').rstrip('\r\n')
            self.log.write(line + '\n')
            self.log.flush()
            with self.cond:
                self.lines.append(line)
                self.cond.notify_all()
        with self.cond:
            self.cond.notify_all()

    def wait(self, pattern, timeout, start=0):
        """Index of the first line from @start matching @pattern, or None"""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        i = start
        with self.cond:
            while True:
                while i < len(self.lines):
                    if rx.search(self.lines[i]):
                        return i
                    i += 1
                left = deadline - time.monotonic()
                if left <= 0 or self.p.poll() is not None:
                    return None
                self.cond.wait(left)

    def count(self, pattern):
        rx = re.compile(pattern)
        with self.cond:
            return sum(1 for line in self.lines if rx.search(line))

    def send(self, text):
        self.p.stdin.write(text.encode() + b'\r')
        self.p.stdin.flush()

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
        self.log.close()

    def unimp(self):
        """Unmodelled Wi-Fi registers the guest touched"""
        try:
            text = self.unimp_path.read_text(errors='replace')
        except FileNotFoundError:
            return []
        return sorted({line for line in text.splitlines()
                       if line.startswith('esp_wifi:')})


# Host side services

HTTP_PORT = 8070


class HttpServer:
    def __init__(self, port):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'hello from the QEMU Wi-Fi test\n'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.srv = http.server.ThreadingHTTPServer(('127.0.0.1', port),
                                                   Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def close(self):
        self.srv.shutdown()


class Sink:
    """TCP and UDP discard server for iperf, counting bytes per second"""

    def __init__(self):
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(('127.0.0.1', 0))
        self.tcp.listen(4)
        self.port = self.tcp.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', self.port))
        self.bytes = {'tcp': {}, 'udp': {}}
        self.running = True
        threading.Thread(target=self._tcp, daemon=True).start()
        threading.Thread(target=self._udp, daemon=True).start()

    def _add(self, proto, n):
        sec = int(time.monotonic())
        self.bytes[proto][sec] = self.bytes[proto].get(sec, 0) + n

    def _tcp(self):
        while self.running:
            try:
                conn, _ = self.tcp.accept()
            except OSError:
                return
            threading.Thread(target=self._tcp_conn, args=(conn,),
                             daemon=True).start()

    def _tcp_conn(self, conn):
        with conn:
            while True:
                data = conn.recv(65536)
                if not data:
                    return
                self._add('tcp', len(data))

    def _udp(self):
        while self.running:
            try:
                data, _ = self.udp.recvfrom(65536)
            except OSError:
                return
            self._add('udp', len(data))

    def longest_gap(self, proto, start, end):
        """Longest run of seconds without traffic between @start and @end"""
        gap = best = 0
        for sec in range(int(start) + 1, int(end)):
            gap = 0 if self.bytes[proto].get(sec) else gap + 1
            best = max(best, gap)
        return best

    def close(self):
        self.running = False
        self.tcp.close()
        self.udp.close()


class DhcpClient:
    """
    A host on the other end of a QEMU dgram netdev, asking for a DHCP lease.
    Each UDP datagram carries one Ethernet frame.
    """

    MAC = bytes.fromhex('020000000a01')

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        self.qemu_port = free_port(socket.SOCK_DGRAM)
        self.lease = None
        self.xid = 0x51454d55

    def netdev(self):
        return (f'dgram,local.type=inet,local.host=127.0.0.1,'
                f'local.port={self.qemu_port},remote.type=inet,'
                f'remote.host=127.0.0.1,remote.port={self.port}')

    @staticmethod
    def _csum(data):
        if len(data) % 2:
            data += b'\0'
        s = sum(struct.unpack(f'!{len(data) // 2}H', data))
        s = (s >> 16) + (s & 0xffff)
        s += s >> 16
        return ~s & 0xffff

    def _frame(self, msgtype, extra=b''):
        opts = bytes([53, 1, msgtype]) + extra + b'\xff'
        bootp = struct.pack('!BBBBIHH4s4s4s4s16s64s128s4s', 1, 1, 6, 0,
                            self.xid, 0, 0x8000, b'\0' * 4, b'\0' * 4,
                            b'\0' * 4, b'\0' * 4, self.MAC, b'', b'',
                            bytes([99, 130, 83, 99])) + opts
        udp = struct.pack('!HHHH', 68, 67, 8 + len(bootp), 0) + bootp
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 64,
                         17, 0, b'\0' * 4, b'\xff' * 4)
        ip = ip[:10] + struct.pack('!H', self._csum(ip)) + ip[12:]
        return b'\xff' * 6 + self.MAC + b'\x08\x00' + ip + udp

    def _reply(self, timeout):
        self.sock.settimeout(timeout)
        try:
            frame, _ = self.sock.recvfrom(4096)
        except socket.timeout:
            return None
        if len(frame) < 282 or frame[12:14] != b'\x08\x00' or frame[23] != 17:
            return None
        ihl = (frame[14] & 0xf) * 4
        udp = frame[14 + ihl:]
        if struct.unpack('!H', udp[2:4])[0] != 68:
            return None
        bootp = udp[8:]
        if bootp[0] != 2 or struct.unpack('!I', bootp[4:8])[0] != self.xid:
            return None
        opts, i, parsed = bootp[240:], 0, {}
        while i < len(opts) and opts[i] != 255:
            if opts[i] == 0:
                i += 1
                continue
            parsed[opts[i]] = opts[i + 2:i + 2 + opts[i + 1]]
            i += 2 + opts[i + 1]
        return bootp[16:20], parsed

    def run(self, timeout):
        deadline = time.monotonic() + timeout
        dest = ('127.0.0.1', self.qemu_port)
        while time.monotonic() < deadline:
            self.sock.sendto(self._frame(1), dest)
            end = time.monotonic() + 2
            while time.monotonic() < end:
                r = self._reply(0.5)
                if not r or r[1].get(53) != b'\x02':
                    continue
                yiaddr, opts = r
                req = bytes([50, 4]) + yiaddr + bytes([54, 4]) + opts[54]
                self.sock.sendto(self._frame(3, req), dest)
                end2 = time.monotonic() + 5
                while time.monotonic() < end2:
                    r = self._reply(0.5)
                    if r and r[1].get(53) == b'\x05':
                        self.lease = socket.inet_ntoa(r[0])
                        return self.lease
        return None

    def close(self):
        self.sock.close()


# Building

class Builder:
    def __init__(self, work, rebuild):
        self.work = work
        self.rebuild = rebuild

    def build(self, idf, chip, name, src, sdkconfig='', patch=None):
        """Copy @src, add @sdkconfig defaults, build; return the flash image"""
        tag = idf_tag(idf)
        proj = self.work / 'build' / tag / chip / name
        flash = proj / 'build' / 'flash.bin'
        if flash.exists() and not self.rebuild:
            return flash
        if proj.exists():
            shutil.rmtree(proj)
        shutil.copytree(src, proj, ignore=shutil.ignore_patterns(
            'build', 'managed_components', 'sdkconfig', 'dependencies.lock'))
        for f in proj.glob('sdkconfig.ci*'):
            f.unlink()
        # The chip specific file is applied last, so override it too
        for defaults in (proj / 'sdkconfig.defaults',
                         proj / f'sdkconfig.defaults.{chip}'):
            old = defaults.read_text() if defaults.exists() else ''
            defaults.write_text(old + COMMON_SDKCONFIG + sdkconfig)
        if patch:
            patch(proj)
        # esptool v5 (IDF >= 6.0) renamed merge_bin and its options
        merge = f'python -m esptool --chip {chip}'
        script = (f'. "{idf}/export.sh" > /dev/null 2>&1 && '
                  f'idf.py set-target {chip} > /dev/null && idf.py build && '
                  f'cd build && ({merge} merge-bin --pad-to-size 4MB '
                  f'-o flash.bin @flash_args || {merge} merge_bin '
                  f'--fill-flash-size 4MB -o flash.bin @flash_args)')
        env = dict(os.environ, IDF_PATH=str(idf))
        with open(proj / 'build.log', 'w') as log:
            rc = subprocess.call(['bash', '-c', script], cwd=proj, env=env,
                                 stdout=log, stderr=subprocess.STDOUT)
        if rc or not flash.exists():
            raise RuntimeError(f'build failed, see {proj / "build.log"}')
        return flash


# Tests. Each returns a Result; @q collects the QEMU instances to report.

def example(idf, path):
    return idf / 'examples' / path


def t_station(ctx, idf, chip, q):
    flash = ctx.builder.build(idf, chip, 'station',
                              example(idf, 'wifi/getting_started/station'), '''
CONFIG_ESP_WIFI_SSID="Espressif"
CONFIG_ESP_WIFI_PASSWORD=""
CONFIG_ESP_WIFI_AUTH_OPEN=y
''')
    vm = Qemu(ctx, chip, flash, 'station', 'user', '02:00:00:00:01:00')
    q.append(vm)
    return Result(vm.wait(r'got ip:', 90) is not None, 'got ip')


def t_station_wpa2(ctx, idf, chip, q):
    src = example(idf, 'wifi/getting_started/station')
    flash = ctx.builder.build(idf, chip, 'station_wpa2', src, f'''
CONFIG_ESP_WIFI_SSID="Espressif"
CONFIG_ESP_WIFI_PASSWORD="{PSK}"
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y
''')
    vm = Qemu(ctx, chip, flash, 'station_wpa2', 'user', '02:00:00:00:01:00',
              wifi=[('ap0-psk', PSK)])
    q.append(vm)
    return Result(vm.wait(r'got ip:', 90) is not None, 'WPA2-PSK, got ip')


def t_http_request(ctx, idf, chip, q):
    # slirp connects the guest's 10.0.2.2 to the host's loopback
    srv = HttpServer(HTTP_PORT)

    def patch(proj):
        main = proj / 'main' / 'http_request_example_main.c'
        text = main.read_text()
        text = re.sub(r'#define WEB_SERVER "[^"]*"',
                      '#define WEB_SERVER "10.0.2.2"', text)
        text = re.sub(r'#define WEB_PORT "[^"]*"',
                      '#define WEB_PORT CONFIG_TEST_WEB_PORT', text)
        main.write_text(text)
        (proj / 'main' / 'Kconfig.projbuild').write_text(
            'menu "Test"\n    config TEST_WEB_PORT\n        string "port"\n'
            '        default "80"\nendmenu\n')

    try:
        flash = ctx.builder.build(idf, chip, 'http_request',
                                  example(idf, 'protocols/http_request'), f'''
CONFIG_EXAMPLE_CONNECT_WIFI=y
CONFIG_EXAMPLE_CONNECT_ETHERNET=n
CONFIG_EXAMPLE_WIFI_SSID="Espressif"
CONFIG_EXAMPLE_WIFI_PASSWORD=""
CONFIG_EXAMPLE_WIFI_AUTH_OPEN=y
CONFIG_EXAMPLE_CONNECT_IPV6=n
CONFIG_TEST_WEB_PORT="{HTTP_PORT}"
''', patch=patch)
        vm = Qemu(ctx, chip, flash, 'http_request', 'user',
                  '02:00:00:00:01:00')
        q.append(vm)
        ok = vm.wait(r'HTTP/1\.[01] 200', 120) is not None
        return Result(ok, 'HTTP 200 from the host')
    finally:
        srv.close()


def t_scan(ctx, idf, chip, q):
    flash = ctx.builder.build(idf, chip, 'scan', example(idf, 'wifi/scan'))
    aps = [('qemu-ap1', 1, -40, None), ('qemu-ap6', 6, -55, PSK),
           ('qemu-ap11', 11, -70, None)]
    props = []
    for i, (ssid, ch, rssi, psk) in enumerate(aps):
        props += [(f'ap{i}-ssid', ssid), (f'ap{i}-channel', ch),
                  (f'ap{i}-rssi', rssi)]
        if psk:
            props.append((f'ap{i}-psk', psk))
    vm = Qemu(ctx, chip, flash, 'scan', 'user', '02:00:00:00:01:00',
              wifi=props)
    q.append(vm)
    if vm.wait(r'Total APs scanned', 90) is None:
        return Result(False, 'scan did not finish')
    vm.wait(r'scan: Channel', 5)
    time.sleep(1)
    found = {}
    cur = None
    for line in vm.lines:
        m = re.search(r'scan: (SSID|RSSI|Channel|Authmode)\s+(\S+)', line)
        if not m:
            continue
        if m.group(1) == 'SSID':
            cur = m.group(2)
            found.setdefault(cur, []).append({})
        elif cur:
            found[cur][-1][m.group(1)] = m.group(2)
    errors = []
    for ssid, ch, rssi, psk in aps:
        entries = found.get(ssid, [])
        if len(entries) != 1:
            errors.append(f'{ssid} listed {len(entries)} times')
            continue
        e = entries[0]
        if e.get('Channel') != str(ch) or e.get('RSSI') != str(rssi):
            errors.append(f'{ssid}: {e}')
        if (psk is not None) != (e.get('Authmode') == 'WIFI_AUTH_WPA2_PSK'):
            errors.append(f'{ssid}: auth {e.get("Authmode")}')
    return Result(not errors, '; '.join(errors) or
                  '3 APs on channels 1/6/11 with configured RSSI and auth')


def _softap(ctx, idf, chip, q, name, psk):
    client = DhcpClient()
    try:
        flash = ctx.builder.build(idf, chip, name,
                                  example(idf, 'wifi/getting_started/softAP'),
                                  f'''
CONFIG_ESP_WIFI_SSID="qemu-softap"
CONFIG_ESP_WIFI_PASSWORD="{psk}"
CONFIG_ESP_WIFI_CHANNEL=6
# WPA2 with PMF required, not WPA3-SAE, which the model does not emulate
CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n
''')
        vm = Qemu(ctx, chip, flash, name, client.netdev(),
                  '02:00:00:00:01:00',
                  wifi=[('client-psk', psk)] if psk else [])
        q.append(vm)
        if vm.wait(r'wifi_init_softap finished', 90) is None:
            return Result(False, 'SoftAP did not start')
        lease = client.run(60)
        joined = vm.wait(r'station .* join', 5) is not None
        ok = lease is not None and joined
        return Result(ok, f'client joined={joined}, DHCP lease {lease}')
    finally:
        client.close()


def t_softap(ctx, idf, chip, q):
    return _softap(ctx, idf, chip, q, 'softap', '')


def t_softap_wpa2(ctx, idf, chip, q):
    return _softap(ctx, idf, chip, q, 'softap_wpa2', PSK)


def _espnow_pair(ctx, chip, flash, name):
    """Two instances joined by a pair of UDP sockets"""
    ports = [free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_DGRAM)]
    vms = []
    for i in (0, 1):
        nic = (f'dgram,local.type=inet,local.host=127.0.0.1,'
               f'local.port={ports[i]},remote.type=inet,'
               f'remote.host=127.0.0.1,remote.port={ports[1 - i]}')
        vms.append(Qemu(ctx, chip, flash, f'{name}{i + 1}', nic,
                        f'02:00:00:00:0{i + 1}:00'))
        time.sleep(1)
    return vms


def t_espnow(ctx, idf, chip, q):
    flash = ctx.builder.build(idf, chip, 'espnow', example(idf, 'wifi/espnow'),
                              '''
CONFIG_ESPNOW_SEND_COUNT=20
CONFIG_ESPNOW_SEND_DELAY=200
''')
    vms = _espnow_pair(ctx, chip, flash, 'espnow')
    q.extend(vms)
    # One side becomes the unicast sender; both must receive the other's data
    rx = [vm.wait(r'Receive \d+th (unicast|broadcast) data from', 90)
          for vm in vms]
    uni = vms[0].wait(r'Receive \d+th unicast data', 60) is not None or \
        vms[1].wait(r'Receive \d+th unicast data', 60) is not None
    errors = sum(vm.count(r'Send error') for vm in vms)
    ok = all(r is not None for r in rx) and uni and errors == 0
    return Result(ok, f'both receive={all(r is not None for r in rx)}, '
                      f'unicast={uni}, send errors={errors}')


def _espnow_ack(ctx, idf, chip, q, name, encrypt):
    flash = ctx.builder.build(idf, chip, name, APPS / 'espnow_ack',
                              'CONFIG_ESPNOW_TEST_ENCRYPT=y\n' if encrypt
                              else '')
    vms = _espnow_pair(ctx, chip, flash, name)
    q.extend(vms)
    res = [vm.wait(r'ESPNOW_TEST (PASS|FAIL)', 150) for vm in vms]
    detail = []
    for vm in vms:
        i = vm.wait(r'ESPNOW_TEST peer_ok', 1)
        detail.append(vm.lines[i].split('ESPNOW_TEST ')[1] if i is not None
                      else 'no result')
    ok = all(r is not None and 'PASS' in vm.lines[r]
             for r, vm in zip(res, vms))
    return Result(ok, ' | '.join(detail))


def t_espnow_ack(ctx, idf, chip, q):
    return _espnow_ack(ctx, idf, chip, q, 'espnow_ack', False)


def t_espnow_ack_encrypted(ctx, idf, chip, q):
    return _espnow_ack(ctx, idf, chip, q, 'espnow_ack_enc', True)


def t_iperf(ctx, idf, chip, q):
    sink = Sink()
    try:
        flash = ctx.builder.build(idf, chip, 'iperf',
                                  example(idf, 'wifi/iperf'), '''
CONFIG_ESP_TASK_WDT_EN=n
''')
        vm = Qemu(ctx, chip, flash, 'iperf', 'user', '02:00:00:00:01:00')
        q.append(vm)
        if vm.wait(r'iperf>|esp>|Type .help', 90) is None:
            return Result(False, 'no console')
        time.sleep(2)
        vm.send('sta_connect Espressif')
        if vm.wait(r'(got ip|GOT_IP|sta ip:|IP_EVENT_STA_GOT_IP)', 60) is None:
            return Result(False, 'no IP')
        time.sleep(2)
        d = ctx.iperf_time
        interval = re.compile(r'\]\s+(\d+\.\d+)-\s*(\d+\.\d+) sec\s+'
                              r'(\d+\.\d+) MBytes\s+(\d+\.\d+) Mbits/sec')
        results = []
        for proto, opt in (('tcp', ''), ('udp', '-u')):
            mark = len(vm.lines)
            start = time.monotonic()
            vm.send(f'iperf -c 10.0.2.2 -p {sink.port} -t {d} -i 5 {opt}')
            done = vm.wait(rf'\]\s+{d - 5}\.0-\s*{d}\.0 sec', d + 60, mark)
            end = time.monotonic()
            rates = [float(m.group(4)) for line in vm.lines[mark:]
                     if (m := interval.search(line)) and
                     float(m.group(2)) - float(m.group(1)) <= 5.0]
            gap = sink.longest_gap(proto, start + 3, end - 1)
            total = sum(sink.bytes[proto].values())
            ok = (done is not None and len(rates) >= d // 5 and
                  min(rates, default=0) > 0 and gap <= 3)
            results.append((proto, ok, rates, total, gap))
            time.sleep(3)
        detail = '; '.join(
            f'{p} {"ok" if ok else "FAIL"}: {len(r)} intervals '
            f'{min(r, default=0):.1f}-{max(r, default=0):.1f} Mbit/s, '
            f'host got {t / 1e6:.1f} MB, longest gap {g}s'
            for p, ok, r, t, g in results)
        return Result(all(ok for _, ok, *_ in results), detail)
    finally:
        sink.close()


def t_restart(ctx, idf, chip, q):
    flash = ctx.builder.build(idf, chip, 'restart', APPS / 'restart')
    vm = Qemu(ctx, chip, flash, 'restart', 'user', '02:00:00:00:01:00',
              reboot=True)
    q.append(vm)
    ok = vm.wait(r'RESTART_TEST PASS', 150) is not None
    return Result(ok, f'restarted={vm.count("RESTART_TEST restarting")}, '
                      f'connections={vm.count("RESTART_TEST boot=")}')


TESTS = {
    'station': t_station,
    'station_wpa2': t_station_wpa2,
    'http_request': t_http_request,
    'scan': t_scan,
    'softap': t_softap,
    'softap_wpa2': t_softap_wpa2,
    'espnow': t_espnow,
    'espnow_ack': t_espnow_ack,
    'espnow_ack_encrypted': t_espnow_ack_encrypted,
    'iperf': t_iperf,
    'restart': t_restart,
}


class Context:
    pass


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf', action='append', required=True, type=Path,
                   help='ESP-IDF checkout (repeat for several versions)')
    p.add_argument('--chips', default='esp32,esp32c3')
    p.add_argument('--tests', default=','.join(TESTS))
    p.add_argument('--qemu-dir', type=Path,
                   default=HERE.parent.parent / 'build')
    p.add_argument('--work', type=Path, default=Path('esp_wifi_work'))
    p.add_argument('--report', type=Path)
    p.add_argument('--rebuild', action='store_true')
    p.add_argument('--icount', action='store_true',
                   help='run QEMU with -icount shift=auto')
    p.add_argument('--iperf-time', type=int, default=60)
    args = p.parse_args()

    ctx = Context()
    ctx.qemu_dir = args.qemu_dir.resolve()
    ctx.icount = args.icount
    ctx.iperf_time = args.iperf_time
    work = args.work.resolve()
    ctx.builder = Builder(work, args.rebuild)

    results = []
    for idf in args.idf:
        idf = idf.expanduser().resolve()
        tag = idf_tag(idf)
        for chip in args.chips.split(','):
            for name in args.tests.split(','):
                ctx.run_dir = work / 'runs' / tag / chip / name
                if ctx.run_dir.exists():
                    shutil.rmtree(ctx.run_dir)
                ctx.run_dir.mkdir(parents=True)
                vms = []
                t0 = time.monotonic()
                try:
                    r = TESTS[name](ctx, idf, chip, vms)
                except Exception as e:
                    r = Result(False, f'error: {e}')
                finally:
                    for vm in vms:
                        vm.stop()
                unimp = sorted({u for vm in vms for u in vm.unimp()})
                status = 'PASS' if r.ok else 'FAIL'
                print(f'{status} {tag:8} {chip:8} {name:22} '
                      f'{time.monotonic() - t0:5.0f}s  {r.detail}',
                      flush=True)
                results.append((tag, chip, name, r, unimp))

    if args.report:
        write_report(args.report, results, args)
    sys.exit(0 if all(r.ok for *_, r, _ in results) else 1)


def write_report(path, results, args):
    tags = sorted({t for t, *_ in results}, reverse=True)
    chips = [c for c in args.chips.split(',')]
    names = args.tests.split(',')
    cells = {(t, c, n): r for t, c, n, r, _ in results}
    out = ['# Wi-Fi emulation test matrix', '',
           f'QEMU: `{args.qemu_dir}`' + (' with -icount' if args.icount
                                         else ''), '']
    header = ['Test'] + [f'{t} {c}' for t in tags for c in chips]
    out.append('| ' + ' | '.join(header) + ' |')
    out.append('|' + '---|' * len(header))
    for n in names:
        row = [n]
        for t in tags:
            for c in chips:
                r = cells.get((t, c, n))
                row.append('' if r is None else ('PASS' if r.ok else 'FAIL'))
        out.append('| ' + ' | '.join(row) + ' |')
    out += ['', '## Details', '']
    for t, c, n, r, unimp in results:
        out.append(f'- **{t} {c} {n}**: {"PASS" if r.ok else "FAIL"}, '
                   f'{r.detail}')
        if unimp and not r.ok:
            out.append('  - unmodelled registers:')
            out += [f'    - `{u}`' for u in unimp[:40]]
    path.write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
