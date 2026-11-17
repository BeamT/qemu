#!/usr/bin/env python3
#
# Wi-Fi emulation on a real medium: the wmediumd backend of the Wi-Fi model
# (the "air" property), with real WPA2/CCMP peers.
#
# Two media:
#
#   --medium hwsim   wmediumd (e.g. the Cuttlefish build) with
#                    mac80211_hwsim radios running hostapd, dnsmasq and
#                    wpa_supplicant in network namespaces. Needs root, the
#                    mac80211_hwsim module and those programs; skipped
#                    (exit 0) when they are missing.
#   --medium fake    tests/esp_wifi/fake_wmediumd.py, with a second QEMU
#                    instance as the peer. No root needed.
#
# Tests, each against the ESP-IDF app in tests/esp_wifi/apps/air:
#
#   station   the guest scans and sees the WPA2 network on its channel, is
#             refused with a wrong passphrase (4-way handshake failure, not
#             a timeout), joins with the right one, gets a DHCP lease, and
#             pings the access point host and is pinged by it. On hwsim
#             the access point offers PMF: the guest must negotiate it,
#             act on a protected deauthentication and send one hostapd
#             accepts.
#   softap    the guest runs a WPA2 SoftAP on channel 6; a client joins it,
#             gets a lease from the guest's DHCP server and fetches
#             http://192.168.4.1/ from the guest.
#   apsta     the guest runs its HT40 SoftAP on the home network's channel
#             (the radio then sits on the centre of the 40 MHz pair) and its
#             station joins the home network there, gets a lease and pings
#             the host; a client of the SoftAP fetches 24 KB from the guest,
#             many TCP segments under a block ack agreement. On hwsim the
#             client is an HT station with power save on, like a phone.
#
# Build as yourself first (root has no IDF environment), then run as root:
#
#   tests/esp_wifi/run_air.py --idf ~/esp/esp-idf-v6.1 --build-only
#   sudo tests/esp_wifi/run_air.py --idf ~/esp/esp-idf-v6.1 --medium hwsim \
#       --wmediumd /usr/lib/cuttlefish-common/bin/wmediumd
#
# The run without the air property (no regression) is
# tests/esp_wifi/run_matrix.py.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_matrix as rm                                         # noqa: E402

PSK = 'focal-lab-wifi'
HOME_SSID = 'QemuAirHome'
HOME_NET = '192.168.77'
DUO_SSID = 'QemuAirDuo'
# Clear of the hwsim radios, 02:00:00:00:NN:00
GUEST_MAC = '02:00:00:00:10:00'
PEER_MAC = '02:00:00:00:20:00'      # the second guest of --medium fake
WRONG_PSK_REASONS = (15, 204)       # 4-way handshake timeout / failure


class Skip(Exception):
    pass


class Fail(Exception):
    pass


def log(msg):
    print(f'run_air: {msg}', flush=True)


class Procs:
    """Background processes, stopped in reverse order"""

    def __init__(self, work):
        self.work = work
        self.procs = []

    def start(self, name, cmd, **kw):
        out = open(self.work / f'{name}.log', 'w')
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, **kw)
        self.procs.append((name, p, out))
        return p

    def stop_one(self, name):
        for n, p, out in self.procs:
            if n == name and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                p.wait(5)

    def stop(self):
        for name, p, out in reversed(self.procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
            out.close()
        self.procs = []


def wait_file(path, pattern, timeout):
    rx = re.compile(pattern)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            if rx.search(Path(path).read_text(errors='replace')):
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


class Guest(rm.Qemu):
    """The air test app in QEMU, its Wi-Fi on the medium at @chardev"""

    def __init__(self, ctx, flash, name, mac, chardev):
        super().__init__(ctx, ctx.chip, flash, name, None, mac,
                         extra=['-chardev', f'socket,id=air,{chardev}',
                                '-trace', 'esp_wifi_wmd_connected',
                                '-msg', 'timestamp=on'] +
                         [a for t in ctx.trace for a in ('-trace', t)],
                         wifi=[('air', 'air')])
        if self.wait(r'AIR ready', 90) is None:
            raise Fail(f'{name}: the app did not start')

    def ask(self, text, pattern, timeout):
        """Send a console command, return the match of the first answer"""
        start = len(self.lines)
        self.send(text)
        i = self.wait(pattern, timeout, start)
        return None if i is None else re.search(pattern, self.lines[i])


# Tests, on whatever medium the context set up

def t_station(ctx, guest):
    """
    Against the network the medium provides: ctx.home_ssid on
    ctx.home_channel, whose host ctx.home_host can ping us (ctx.home_ping)
    """
    res = {}
    seen = None
    for _ in range(3):
        start = len(guest.lines)
        if guest.ask('scan', r'AIR scan done', 60) is None:
            raise Fail('scan did not finish')
        for line in guest.lines[start:]:
            m = re.search(rf'AIR scan ssid={ctx.home_ssid} channel=(\d+)',
                          line)
            if m:
                seen = int(m.group(1))
        if seen:
            break
    if seen != ctx.home_channel:
        raise Fail(f'scan: {ctx.home_ssid} on channel {seen}, expected '
                   f'{ctx.home_channel}')
    res['scan'] = f'channel {seen}'

    m = guest.ask(f'join {ctx.home_ssid} wrong-passphrase',
                  r'AIR sta (disconnected reason=(\d+)|got ip)', 90)
    if not m or not m.group(2) or int(m.group(2)) not in WRONG_PSK_REASONS:
        raise Fail(f'wrong passphrase: {m.group(0) if m else "no answer"}')
    res['wrong passphrase'] = f'reason {m.group(2)}'
    guest.send('leave')
    time.sleep(2)

    t0 = time.monotonic()
    m = guest.ask(f'join {ctx.home_ssid} {PSK}',
                  r'AIR sta got ip (\d+\.\d+\.\d+\.\d+)', 90)
    if not m:
        raise Fail('no DHCP lease')
    ip = m.group(1)
    res['join'] = f'{time.monotonic() - t0:.1f} s to {ip}'

    m = guest.ask(f'ping {ctx.home_host} 5', r'AIR ping ok=(\d+)', 60)
    if not m or int(m.group(1)) < 3:
        raise Fail(f'guest ping: {m.group(0) if m else "no answer"}')
    res['ping out'] = f'{m.group(1)}/5'
    ok = ctx.home_ping(ip)
    if ok < 3:
        raise Fail(f'ping to the guest: {ok}/5')
    res['ping in'] = f'{ok}/5'

    if ctx.home_pmf:
        # Protected management frames, both ways
        if not ctx.home_pmf.negotiated():
            raise Fail('PMF: not negotiated')
        n = len(guest.lines)
        ctx.home_pmf.deauth()
        i = guest.wait(r'AIR sta disconnected reason=\d+', 10, n)
        if i is None:
            raise Fail('PMF: protected deauthentication ignored')
        reason = re.search(r'reason=(\d+)', guest.lines[i]).group(1)
        if guest.ask(f'join {ctx.home_ssid} {PSK}', r'AIR sta got ip',
                     90) is None:
            raise Fail('PMF: no lease after rejoining')
        mark = ctx.home_pmf.mark()
        guest.send('leave')
        if not ctx.home_pmf.left(mark, 5):
            raise Fail('PMF: the access point ignored the guest leaving')
        res['pmf'] = f'deauth received (reason {reason}) and sent'
    else:
        guest.send('leave')
    time.sleep(1)
    return res


def t_softap(ctx, guest):
    res = {}
    # Setting the config restarts the AP: wait for the second start, and
    # for its first beacons, so the join time does not include a client
    # scan that ran before the AP was on the air
    if guest.ask(f'ap {DUO_SSID} {PSK} 6', r'AIR ap ESP_OK', 30) is None:
        raise Fail('SoftAP did not start')
    time.sleep(2)
    t0 = time.monotonic()
    lease = ctx.duo_join()
    res['join'] = f'{time.monotonic() - t0:.1f} s, lease {lease}'
    body = ctx.duo_get('http://192.168.4.1/')
    if 'AIR hello' not in body:
        raise Fail(f'HTTP GET: {body!r}')
    res['http'] = 'GET / answered'
    return res


BIG_LEN = 24 * 1024


def t_apsta(ctx, guest):
    res = {}
    ch = ctx.home_channel
    ctx.home_up()
    if guest.ask(f'ap {DUO_SSID} {PSK} {ch}', r'AIR ap ESP_OK', 30) is None:
        raise Fail('SoftAP did not start')
    time.sleep(2)
    t0 = time.monotonic()
    m = guest.ask(f'join {ctx.home_ssid} {PSK}',
                  r'AIR sta got ip (\d+\.\d+\.\d+\.\d+)', 90)
    if not m:
        raise Fail(f'station: no lease from {ctx.home_ssid} on channel {ch} '
                   'while the HT40 SoftAP runs there')
    res['join'] = f'{time.monotonic() - t0:.1f} s to {m.group(1)}'
    m = guest.ask(f'ping {ctx.home_host} 5', r'AIR ping ok=(\d+)', 60)
    if not m or int(m.group(1)) < 3:
        raise Fail(f'station ping: {m.group(0) if m else "no answer"}')
    res['ping'] = f'{m.group(1)}/5'
    lease = ctx.duo_join()
    res['client'] = f'lease {lease}'
    body = ctx.duo_get('http://192.168.4.1/big')
    if len(body) != BIG_LEN:
        raise Fail(f'GET /big: {len(body)} of {BIG_LEN} bytes')
    res['bulk'] = f'{len(body)} bytes'
    guest.send('leave')
    time.sleep(1)
    return res


# The hwsim medium

def need(prog):
    path = shutil.which(prog)
    if not path:
        raise Skip(f'{prog} not found')
    return path


def find_wmediumd(arg):
    cands = [arg] if arg else []
    user = os.environ.get('SUDO_USER')
    if user:
        cands.append(os.path.expanduser(f'~{user}/.local/lib/focal-lab/'
                                        'wmediumd'))
    cands += ['/usr/lib/cuttlefish-common/bin/wmediumd',
              shutil.which('wmediumd')]
    for c in cands:
        if c and os.access(c, os.X_OK):
            return c
    raise Skip('wmediumd not found (--wmediumd)')


def sh(*cmd, check=True, **kw):
    return subprocess.run(cmd, check=check, capture_output=True, text=True,
                          **kw)


def hwsim_radios():
    """[(phy, netdev, mac)] of the mac80211_hwsim radios"""
    radios = []
    for phy in sorted(Path('/sys/class/ieee80211').iterdir(),
                      key=lambda p: int(p.name[3:])):
        drv = phy / 'device' / 'driver'
        if not drv.exists() or 'hwsim' not in os.path.realpath(drv):
            continue
        nets = list((phy / 'device' / 'net').iterdir())
        if nets:
            mac = (nets[0] / 'address').read_text().strip()
            radios.append((phy.name, nets[0].name, mac))
    return radios


class HwsimMedium:
    def __init__(self, ctx, args):
        if os.geteuid() != 0:
            raise Skip('needs root (sudo)')
        self.wmediumd = find_wmediumd(args.wmediumd)
        for prog in ('iw', 'ip', 'hostapd', 'hostapd_cli', 'dnsmasq',
                     'wpa_supplicant', 'busybox', 'curl', 'ping', 'modprobe'):
            need(prog)
        if Path('/sys/module/mac80211_hwsim').exists():
            raise Skip('mac80211_hwsim is already loaded; unload it first')
        if sh('modprobe', '-n', 'mac80211_hwsim', check=False).returncode:
            raise Skip('no mac80211_hwsim module')
        self.ctx = ctx
        self.work = ctx.run_dir
        self.procs = Procs(self.work)
        self.netns = []
        self.loaded = False

    def ns(self, name, *cmd):
        return ['ip', 'netns', 'exec', name, *cmd]

    def setup(self):
        ctx = self.ctx
        sh('modprobe', 'mac80211_hwsim', 'radios=2')
        self.loaded = True
        time.sleep(1)
        radios = hwsim_radios()
        if len(radios) < 2:
            raise Fail(f'expected 2 hwsim radios, found {radios}')
        (self.ap_phy, self.ap_if, ap_mac), (self.sta_phy, self.sta_if,
                                            sta_mac) = radios[:2]
        for name, phy in (('airap', self.ap_phy), ('airsta', self.sta_phy)):
            sh('ip', 'netns', 'add', name)
            self.netns.append(name)
            sh('iw', 'phy', phy, 'set', 'netns', 'name', name)
            sh(*self.ns(name, 'ip', 'link', 'set', 'lo', 'up'))

        # wmediumd, as the lab runs it
        cfg = self.work / 'wmediumd.cfg'
        cfg.write_text('ifaces : {\n  ids = [\n' + ',\n'.join(
            f'    "{m}"'
            for m in (ap_mac, sta_mac, GUEST_MAC)) +
            '\n  ];\n};\n')
        self.api = self.work / 'api.sock'
        p = self.procs.start('wmediumd', [
            self.wmediumd, '-n', '-l', '5', '-u', str(self.work / 'vhost.sock'),
            '-a', str(self.api), '-c', str(cfg),
            '-p', str(self.work / 'air.pcapng')])
        end = time.monotonic() + 10
        while not self.api.exists():
            if p.poll() is not None or time.monotonic() > end:
                raise Fail('wmediumd did not start, see wmediumd.log')
            time.sleep(0.1)
        ctx.chardev = f'path={self.api}'

        # The home network: hostapd and dnsmasq on radio 0
        ifc = self.ap_if
        sh(*self.ns('airap', 'ip', 'addr', 'add', f'{HOME_NET}.1/24',
                    'dev', ifc))
        conf = self.work / 'hostapd.conf'
        conf.write_text(f'''interface={ifc}
driver=nl80211
ssid={HOME_SSID}
hw_mode=g
channel=1
country_code=US
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
ieee80211w=1
wpa_passphrase={PSK}
ctrl_interface={self.work / 'hostapd'}
''')
        self.procs.start('hostapd', self.ns('airap', 'hostapd', str(conf)))
        if not wait_file(self.work / 'hostapd.log', r'AP-ENABLED', 15):
            raise Fail('hostapd did not start, see hostapd.log')
        self.procs.start('dnsmasq', self.ns(
            'airap', 'dnsmasq', '--no-daemon', '--port=0', '--no-resolv',
            '--no-hosts', f'--interface={ifc}', '--bind-interfaces',
            f'--dhcp-range={HOME_NET}.10,{HOME_NET}.50,1h',
            f'--dhcp-leasefile={self.work / "dnsmasq.leases"}'))
        ctx.home_ssid = HOME_SSID
        ctx.home_channel = 1
        ctx.home_host = f'{HOME_NET}.1'
        ctx.home_ping = self.home_ping
        ctx.home_up = lambda: None
        ctx.home_pmf = self
        ctx.duo_join = self.duo_join
        ctx.duo_get = self.duo_get

    # PMF checks against hostapd, which offers PMF (ieee80211w=1)
    def hostapd_cli(self, *args):
        return sh(*self.ns('airap', 'hostapd_cli', '-p',
                           str(self.work / 'hostapd'), '-i', self.ap_if,
                           *args), check=False).stdout

    def negotiated(self):
        return '[MFP]' in self.hostapd_cli('sta', GUEST_MAC)

    def deauth(self):
        self.hostapd_cli('deauthenticate', GUEST_MAC)

    def mark(self):
        return (self.work / 'hostapd.log').read_text().count(
            f'AP-STA-DISCONNECTED {GUEST_MAC}')

    def left(self, mark, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.mark() > mark:
                return True
            time.sleep(0.2)
        return False

    def home_ping(self, ip):
        r = sh(*self.ns('airap', 'ping', '-c', '5', '-W', '2', ip),
               check=False)
        m = re.search(r'(\d+) received', r.stdout)
        return int(m.group(1)) if m else 0

    def duo_join(self):
        ifc = self.sta_if
        conf = self.work / 'wpa_supplicant.conf'
        conf.write_text(f'''ctrl_interface={self.work / "wpa"}
network={{
    ssid="{DUO_SSID}"
    psk="{PSK}"
    key_mgmt=WPA-PSK
    proto=RSN
    pairwise=CCMP
    group=CCMP
    scan_ssid=1
}}
''')
        # A second join (the SoftAP restarted elsewhere): start afresh. The
        # old wpa_supplicant would stay with the vanished BSS, whose
        # connection monitor probes wmediumd acknowledges
        self.procs.stop_one('wpa_supplicant')
        sh(*self.ns('airsta', 'ip', 'addr', 'flush', 'dev', ifc),
           check=False)
        self.procs.start('wpa_supplicant', self.ns(
            'airsta', 'wpa_supplicant', '-t', '-D', 'nl80211', '-i', ifc,
            '-c', str(conf)))
        end = time.monotonic() + 120
        while time.monotonic() < end:
            r = sh(*self.ns('airsta', 'iw', 'dev', ifc, 'link'), check=False)
            if 'Connected to' in r.stdout:
                break
            time.sleep(0.5)
        else:
            raise Fail('wpa_supplicant did not join, see '
                       'wpa_supplicant.log')
        # Like a phone: the access point must buffer for it and wake it
        sh(*self.ns('airsta', 'iw', 'dev', ifc, 'set', 'power_save', 'on'),
           check=False)
        script = self.work / 'udhcpc.sh'
        script.write_text('#!/bin/sh\n[ "$1" = bound ] && '
                          'ip addr add "$ip/$mask" dev "$interface"\n'
                          'exit 0\n')
        script.chmod(0o755)
        r = sh(*self.ns('airsta', 'busybox', 'udhcpc', '-i', ifc, '-n', '-q',
                        '-f', '-t', '10', '-T', '3', '-s', str(script)),
               check=False)
        m = re.search(r'lease of (\S+) obtained', r.stdout + r.stderr)
        if not m:
            raise Fail(f'no lease from the guest: {r.stdout}{r.stderr}')
        return m.group(1)

    def duo_get(self, url):
        r = sh(*self.ns('airsta', 'curl', '-s', '--max-time', '30', url),
               check=False)
        return r.stdout

    def teardown(self):
        self.procs.stop()
        for name in self.netns:
            sh('ip', 'netns', 'del', name, check=False)
        if self.loaded:
            sh('rmmod', 'mac80211_hwsim', check=False)


# The fake medium: a second guest is the peer

class FakeMedium:
    def __init__(self, ctx, args):
        self.ctx = ctx
        self.work = ctx.run_dir
        self.procs = Procs(self.work)
        self.peer = None

    def setup(self):
        ctx = self.ctx
        name = f'esp-air-{os.getpid()}'
        self.procs.start('medium', [
            sys.executable, str(HERE / 'fake_wmediumd.py'), '--api',
            '@' + name, '--station', GUEST_MAC, '--station', PEER_MAC,
            '--pcap', str(self.work / 'air.pcap')])
        time.sleep(1)
        ctx.chardev = f'path={name},abstract=on'
        self.peer = Guest(ctx, ctx.flash, 'peer', PEER_MAC, ctx.chardev)
        ctx.home_channel = 6
        ctx.home_host = '192.168.4.1'
        ctx.home_ping = self.home_ping
        ctx.home_up = self.home_up
        ctx.home_pmf = None
        ctx.duo_join = self.duo_join
        ctx.duo_get = self.duo_get
        ctx.home_ssid = 'QemuAirPeer'
        self.home_up()

    def home_up(self):
        # The peer is also the SoftAP's client, as a station only
        if self.peer.ask(f'ap {self.ctx.home_ssid} {PSK} 6',
                         r'AIR ap started', 30) is None:
            raise Fail('the peer SoftAP did not start')

    def home_ping(self, ip):
        m = self.peer.ask(f'ping {ip} 5', r'AIR ping ok=(\d+)', 60)
        return int(m.group(1)) if m else 0

    def duo_join(self):
        self.peer.ask('sta', r'AIR sta mode', 10)
        m = self.peer.ask(f'join {DUO_SSID} {PSK}',
                          r'AIR sta got ip (\S+)', 90)
        if not m:
            raise Fail('the peer did not join the SoftAP')
        return m.group(1)

    def duo_get(self, url):
        # The peer app has no HTTP client; a ping shows the data path
        m = self.peer.ask('ping 192.168.4.1 5', r'AIR ping ok=(\d+)', 60)
        if not m or int(m.group(1)) < 3:
            return ''
        return 'x' * BIG_LEN if url.endswith('/big') else 'AIR hello'

    def teardown(self):
        if self.peer:
            self.peer.stop()
        self.procs.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument('--idf', required=True, type=Path,
                   help='ESP-IDF checkout to build the test app with')
    p.add_argument('--chip', default='esp32s3',
                   choices=['esp32s3', 'esp32c3'])
    p.add_argument('--medium', default='hwsim', choices=['hwsim', 'fake'])
    p.add_argument('--wmediumd', help='wmediumd binary (default: '
                   '~/.local/lib/focal-lab, cuttlefish-common, PATH)')
    p.add_argument('--tests', default='station,softap,apsta')
    p.add_argument('--qemu-dir', type=Path,
                   default=HERE.parent.parent / 'build')
    p.add_argument('--work', type=Path, default=Path('esp_wifi_work'))
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--rebuild', action='store_true')
    p.add_argument('--trace', action='append', default=[],
                   help='QEMU trace events to log with timestamps in '
                        '<guest>.unimp, e.g. "esp_wifi_wmd_*"')
    args = p.parse_args()

    ctx = types.SimpleNamespace(chip=args.chip, qemu_dir=args.qemu_dir,
                                icount=False, trace=args.trace)
    work = args.work.resolve()
    builder = rm.Builder(work, args.rebuild)
    try:
        ctx.flash = builder.build(args.idf, args.chip, 'air',
                                  HERE / 'apps' / 'air')
    except RuntimeError as e:
        if os.geteuid() == 0:
            log(f'{e}; build as yourself first with --build-only')
        else:
            log(str(e))
        return 1
    if args.build_only:
        log(f'built {ctx.flash}')
        return 0

    # A short path: UNIX socket names are limited to 108 bytes
    ctx.run_dir = Path(tempfile.mkdtemp(prefix='esp-air-'))
    medium = (HwsimMedium if args.medium == 'hwsim' else FakeMedium)
    results = []
    guest = None
    try:
        m = medium(ctx, args)
    except Skip as e:
        log(f'SKIP: {e}')
        return 0
    try:
        m.setup()
        guest = Guest(ctx, ctx.flash, 'guest', GUEST_MAC, ctx.chardev)
        for name in args.tests.split(','):
            fn = {'station': t_station, 'softap': t_softap,
                  'apsta': t_apsta}[name]
            try:
                results.append((name, True, fn(ctx, guest)))
            except Fail as e:
                results.append((name, False, {'error': str(e)}))
    except Fail as e:
        results.append(('setup', False, {'error': str(e)}))
    finally:
        if guest:
            guest.stop()
        m.teardown()

    log(f'medium {args.medium}, chip {args.chip}, logs in {ctx.run_dir}')
    ok = True
    for name, passed, detail in results:
        ok &= passed
        log(f'{"PASS" if passed else "FAIL"} {name}: ' +
            ', '.join(f'{k}: {v}' for k, v in detail.items()))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
