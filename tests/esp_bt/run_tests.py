#!/usr/bin/env python3
#
# Bluetooth LE tests for the Espressif QEMU machines
#
# The NimBLE host runs in the emulated chip and talks HCI over a UART to a
# Rootcanal virtual controller; a Bumble host on a second Rootcanal
# controller plays the phone. Builds the test app for each IDF version and
# chip, then scans, connects, uses GATT, pairs and reconnects.
#
#   pip install -r tests/esp_bt/requirements.txt
#   tests/esp_bt/run_tests.py --idf ~/esp/esp-idf-v5.5.6 --chips esp32
#
# Every run keeps its serial log, the Rootcanal log and an HCI capture per
# controller in the work directory; a Markdown report lists the results.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import asyncio
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

from bumble.att import (ATT_INSUFFICIENT_AUTHENTICATION_ERROR,
                        ATT_INSUFFICIENT_ENCRYPTION_ERROR, ATT_Error)
from bumble.core import UUID
from bumble.device import Device, Peer
from bumble.hci import Address
from bumble.transport import open_transport
from rootcanal.binaries import get_package_binary_resource_path

HERE = Path(__file__).resolve().parent
APP = HERE / 'apps' / 'peripheral'

CHIPS = {
    'esp32': {
        'qemu': 'qemu-system-xtensa',
        'machine': ['-machine', 'esp32', '-m', '4M'],
    },
    'esp32c3': {
        'qemu': 'qemu-system-riscv32',
        'machine': ['-machine', 'esp32c3'],
    },
    'esp32s3': {
        'qemu': 'qemu-system-xtensa',
        'machine': ['-machine', 'esp32s3'],
    },
}

# Applied to every build: flash modes and sizes the QEMU flash model supports
COMMON_SDKCONFIG = '''
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y
'''

DEVICE_NAME = 'qemu-ble'
CENTRAL_ADDRESS = 'F0:F0:F0:F0:F0:F0'


def test_uuid(n):
    return UUID(f'5A9E00{n:02X}-1A2B-4C3D-8E4F-51454D55424C')


SVC_UUID = test_uuid(0)
HELLO_UUID = test_uuid(1)
ECHO_UUID = test_uuid(2)
SECRET_UUID = test_uuid(3)


def idf_tag(idf):
    """Version of an IDF checkout, e.g. v6.1 or v5.5.6"""
    try:
        text = (idf / 'tools' / 'cmake' / 'version.cmake').read_text()
        v = [re.search(rf'IDF_VERSION_{k} (\d+)', text).group(1)
             for k in ('MAJOR', 'MINOR', 'PATCH')]
    except (OSError, AttributeError):
        return idf.name
    return 'v' + '.'.join(v if v[2] != '0' else v[:2])


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Fail(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Fail(what)


# Rootcanal: one controller per HCI connection, all on the same virtual radio

class Rootcanal:
    def __init__(self, run_dir):
        self.run_dir = run_dir
        self.hci_port = free_port()
        self.p = None

    async def start(self):
        log = open(self.run_dir / 'rootcanal.log', 'w')
        ports = [free_port() for _ in range(3)]
        # The binary itself: python -m rootcanal would leave it running
        # when terminated
        self.p = await asyncio.create_subprocess_exec(
            get_package_binary_resource_path('rootcanal'),
            '-enable_hci_sniffer',
            f'-hci_port={self.hci_port}', f'-test_port={ports[0]}',
            f'-link_port={ports[1]}', f'-link_ble_port={ports[2]}',
            stdout=log, stderr=subprocess.STDOUT, cwd=self.run_dir)
        log.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                socket.create_connection(('127.0.0.1', self.hci_port)).close()
                # That connection made a controller; let Rootcanal drop it
                await asyncio.sleep(0.2)
                return
            except OSError:
                await asyncio.sleep(0.1)
        raise Fail('rootcanal did not start')

    async def stop(self):
        if self.p and self.p.returncode is None:
            self.p.terminate()
            await self.p.wait()


# QEMU process with a serial console we can wait on

class Qemu:
    def __init__(self, ctx, chip, image, name, hci_port, uart):
        c = CHIPS[chip]
        self.log_path = ctx.run_dir / f'{name}.log'
        # UART0 is the console; the HCI UART goes on -serial number @uart
        serials = ['-serial', 'mon:stdio']
        serials += ['-serial', 'null'] * (uart - 1)
        serials += ['-serial', f'tcp:127.0.0.1:{hci_port}']
        self.cmd = [str(ctx.qemu_dir / c['qemu']), '-nographic',
                    *c['machine'],
                    '-drive', f'file={image},if=mtd,format=raw',
                    '-no-reboot', *serials]
        if ctx.icount:
            self.cmd += ['-icount', 'shift=auto']
        self.lines = []
        self.changed = asyncio.Condition()
        self.p = None

    async def start(self):
        self.log = open(self.log_path, 'a', errors='replace')
        self.log.write('$ ' + ' '.join(self.cmd) + '\n')
        self.p = await asyncio.create_subprocess_exec(
            *self.cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.reader = asyncio.create_task(self._read())

    async def _read(self):
        async for raw in self.p.stdout:
            line = raw.decode(errors='replace').rstrip('\r\n')
            self.log.write(line + '\n')
            self.log.flush()
            async with self.changed:
                self.lines.append(line)
                self.changed.notify_all()
        async with self.changed:
            self.changed.notify_all()

    async def wait(self, pattern, timeout, start=None):
        """Match of the first line from @start (default: now) or None"""
        rx = re.compile(pattern)
        i = len(self.lines) if start is None else start

        async def scan():
            nonlocal i
            async with self.changed:
                while True:
                    while i < len(self.lines):
                        m = rx.search(self.lines[i])
                        i += 1
                        if m:
                            return m
                    if self.p.returncode is not None:
                        return None
                    await self.changed.wait()
        try:
            return await asyncio.wait_for(scan(), timeout)
        except asyncio.TimeoutError:
            return None

    def mark(self):
        return len(self.lines)

    async def stop(self):
        if self.p and self.p.returncode is None:
            self.p.terminate()
            try:
                await asyncio.wait_for(self.p.wait(), 10)
            except asyncio.TimeoutError:
                self.p.kill()
                await self.p.wait()
        if self.p:
            await self.reader
            self.log.close()


# The phone: a Bumble host on its own Rootcanal controller

class Central:
    def __init__(self, hci_port):
        self.hci_port = hci_port
        self.transport = None

    async def start(self):
        self.transport = await open_transport(
            f'tcp-client:127.0.0.1:{self.hci_port}')
        self.device = Device.with_hci('central', Address(CENTRAL_ADDRESS),
                                      self.transport.source,
                                      self.transport.sink)
        # Pair with the random static address as identity address, the one
        # it connects from, so that the peripheral finds the bond again
        self.device.config.identity_address_type = \
            Address.RANDOM_DEVICE_ADDRESS
        await self.device.power_on()

    async def find(self, timeout):
        """Address the test app advertises from"""
        found = asyncio.get_running_loop().create_future()

        def on_adv(adv):
            name = adv.data.get(0x09)  # complete local name
            if name is not None and str(name) == DEVICE_NAME \
                    and not found.done():
                found.set_result(adv.address)

        self.device.on('advertisement', on_adv)
        await self.device.start_scanning()
        try:
            return await asyncio.wait_for(found, timeout)
        except asyncio.TimeoutError:
            return None
        finally:
            self.device.remove_listener('advertisement', on_adv)
            await self.device.stop_scanning()

    async def connect(self, timeout=30):
        """Connection and GATT proxy for the test service"""
        address = await self.find(timeout)
        check(address is not None, f'no advertisement from {DEVICE_NAME}')
        conn = await self.device.connect(address)
        peer = Peer(conn)
        await peer.discover_services([SVC_UUID])
        svc = [s for s in peer.services if s.uuid == SVC_UUID]
        check(svc, 'test service not found')
        await svc[0].discover_characteristics()
        chrs = {}
        for uuid in (HELLO_UUID, ECHO_UUID, SECRET_UUID):
            found = [c for c in svc[0].characteristics if c.uuid == uuid]
            check(found, f'characteristic {uuid} not found')
            chrs[uuid] = found[0]
        return conn, peer, chrs

    async def stop(self):
        if self.transport:
            await self.transport.close()


# A test bench: Rootcanal, the emulated chip and the central

class Bench:
    def __init__(self, ctx, chip, flash, uart=1):
        self.ctx = ctx
        self.chip = chip
        self.uart = uart
        # A private copy, so that NVS state does not leak between runs
        self.image = ctx.run_dir / 'flash.bin'
        shutil.copy(flash, self.image)
        self.rootcanal = Rootcanal(ctx.run_dir)
        self.vm = None
        self.central = None

    async def __aenter__(self):
        try:
            await self.rootcanal.start()
            await self.boot()
            self.central = Central(self.rootcanal.hci_port)
            await self.central.start()
        except BaseException:
            await self.__aexit__(None, None, None)
            raise
        return self

    async def boot(self):
        """(Re)start the chip on the same flash image"""
        if self.vm:
            await self.vm.stop()
        self.vm = Qemu(self.ctx, self.chip, self.image, 'qemu',
                       self.rootcanal.hci_port, self.uart)
        await self.vm.start()
        m = await self.vm.wait(r'ble: sync, address ([0-9a-f:]+)', 60,
                               start=0)
        check(m is not None, 'NimBLE host did not sync with the controller')
        check(await self.vm.wait(r'ble: advertising', 10, start=0),
              'not advertising')
        self.address = m.group(1)

    async def __aexit__(self, *exc):
        if self.central:
            await self.central.stop()
        if self.vm:
            await self.vm.stop()
        await self.rootcanal.stop()


# Building

class Builder:
    def __init__(self, work, rebuild):
        self.work = work
        self.rebuild = rebuild

    def build(self, idf, chip, name, sdkconfig=''):
        """Copy the test app, add @sdkconfig defaults, build; return flash"""
        proj = self.work / 'build' / idf_tag(idf) / chip / name
        flash = proj / 'build' / 'flash.bin'
        if flash.exists() and not self.rebuild:
            return flash
        if proj.exists():
            shutil.rmtree(proj)
        shutil.copytree(APP, proj, ignore=shutil.ignore_patterns(
            'build', 'managed_components', 'sdkconfig', 'dependencies.lock'))
        defaults = proj / 'sdkconfig.defaults'
        defaults.write_text(defaults.read_text() + COMMON_SDKCONFIG +
                            sdkconfig)
        # esptool v5 (IDF >= 6.0) renamed merge_bin and its options
        merge = f'python -m esptool --chip {chip}'
        script = (f'. "{idf}/export.sh" > /dev/null 2>&1 && '
                  f'idf.py set-target {chip} > /dev/null && idf.py build && '
                  f'cd build && ({merge} merge-bin --pad-to-size 4MB '
                  f'-o flash.bin @flash_args || {merge} merge_bin '
                  f'--fill-flash-size 4MB -o flash.bin @flash_args)')
        env = dict(os.environ, IDF_PATH=str(idf))
        # Do not leak this script's virtual environment into IDF's
        if sys.prefix != sys.base_prefix:
            env.pop('VIRTUAL_ENV', None)
            env['PATH'] = os.pathsep.join(
                p for p in env['PATH'].split(os.pathsep)
                if not p.startswith(sys.prefix))
        with open(proj / 'build.log', 'w') as log:
            rc = subprocess.call(['bash', '-c', script], cwd=proj, env=env,
                                 stdout=log, stderr=subprocess.STDOUT)
        if rc or not flash.exists():
            raise Fail(f'build failed, see {proj / "build.log"}')
        return flash


# Tests. Each raises Fail, or returns a short description of what it saw.

async def t_advertise(ctx, idf, chip):
    """The host syncs over the UART and a scanner sees the advertisement"""
    flash = ctx.builder.build(idf, chip, 'uart1')
    async with Bench(ctx, chip, flash) as b:
        address = await b.central.find(30)
        check(address is not None, f'no advertisement from {DEVICE_NAME}')
        return f'advertising as {address}'


async def t_gatt(ctx, idf, chip):
    """Connect, discover, read, exchange the MTU, write and get notified"""
    flash = ctx.builder.build(idf, chip, 'uart1')
    async with Bench(ctx, chip, flash) as b:
        conn, peer, chrs = await b.central.connect()
        check(await b.vm.wait(r'ble: connected to f0:f0:f0:f0:f0:f0', 10,
                              start=0), 'guest did not see the connection')
        hello = await peer.read_value(chrs[HELLO_UUID])
        check(hello == b'hello from QEMU', f'hello read {hello!r}')

        mtu = await peer.request_mtu(247)
        check(await b.vm.wait(rf'ble: mtu {mtu}\b', 10, start=0),
              f'guest did not see MTU {mtu}')

        notified = asyncio.Queue()
        await peer.subscribe(chrs[ECHO_UUID], notified.put_nowait)
        check(await b.vm.wait(r'ble: subscribed 1', 10, start=0),
              'guest did not see the subscription')
        payload = os.urandom(mtu - 3)
        await peer.write_value(chrs[ECHO_UUID], payload, with_response=True)
        got = await asyncio.wait_for(notified.get(), 10)
        check(got == payload, 'notification differs from the write')
        back = await peer.read_value(chrs[ECHO_UUID])
        check(back == payload, 'echo read differs from the write')
        return f'MTU {mtu}, {len(payload)} byte write notified back'


async def t_pairing(ctx, idf, chip):
    """An encrypted-only read is refused, pairs, then succeeds"""
    flash = ctx.builder.build(idf, chip, 'uart1')
    async with Bench(ctx, chip, flash) as b:
        conn, peer, chrs = await b.central.connect()
        try:
            await peer.read_value(chrs[SECRET_UUID])
            raise Fail('secret read without encryption')
        except ATT_Error as e:
            # NimBLE answers Insufficient Authentication when it has no key
            check(e.error_code in (ATT_INSUFFICIENT_AUTHENTICATION_ERROR,
                                   ATT_INSUFFICIENT_ENCRYPTION_ERROR),
                  f'unencrypted read refused with {e.error_code:#x}')
        await conn.pair()
        check(await b.vm.wait(r'ble: encryption status 0 encrypted 1 '
                              r'bonded 1', 10, start=0),
              'guest did not report a bonded, encrypted link')
        secret = await peer.read_value(chrs[SECRET_UUID])
        check(secret == b'paired with QEMU', f'secret read {secret!r}')
        return 'LE Secure Connections, bonded'


async def t_reconnect(ctx, idf, chip):
    """After pairing, disconnect and reconnect on the stored keys"""
    flash = ctx.builder.build(idf, chip, 'uart1')
    async with Bench(ctx, chip, flash) as b:
        conn, peer, chrs = await b.central.connect()
        await conn.pair()
        for i in range(3):
            mark = b.vm.mark()
            await conn.disconnect()
            check(await b.vm.wait(r'ble: disconnected', 10, start=mark),
                  f'guest did not see disconnect {i}')
            check(await b.vm.wait(r'ble: advertising', 10, start=mark),
                  f'guest did not advertise again after disconnect {i}')
            conn, peer, chrs = await b.central.connect()
            mark = b.vm.mark()
            await conn.encrypt()
            check(await b.vm.wait(r'ble: encryption status 0 encrypted 1 '
                                  r'bonded 1', 10, start=mark),
                  f'reconnection {i} not encrypted with the bond')
            secret = await peer.read_value(chrs[SECRET_UUID])
            check(secret == b'paired with QEMU', f'secret read {secret!r}')
        return '3 reconnections encrypted with the stored LTK'


async def t_reboot(ctx, idf, chip):
    """The bond survives a reboot: NimBLE keeps its keys in NVS"""
    flash = ctx.builder.build(idf, chip, 'uart1')
    async with Bench(ctx, chip, flash) as b:
        conn, peer, chrs = await b.central.connect()
        await conn.pair()
        check(await b.vm.wait(r'ble: encryption status 0 .* bonded 1', 10,
                              start=0), 'not bonded')
        before = b.address
        await b.boot()
        check(b.address == before,
              f'controller address changed: {before} -> {b.address}')
        conn, peer, chrs = await b.central.connect()
        mark = b.vm.mark()
        await conn.encrypt()
        check(await b.vm.wait(r'ble: encryption status 0 encrypted 1 '
                              r'bonded 1', 10, start=mark),
              'bond lost across the reboot')
        return 'bond restored from NVS'


async def t_uart2(ctx, idf, chip):
    """HCI on UART2 instead of UART1"""
    flash = ctx.builder.build(idf, chip, 'uart2',
                              'CONFIG_BT_NIMBLE_TRANSPORT_UART_PORT=2\n')
    async with Bench(ctx, chip, flash, uart=2) as b:
        conn, peer, chrs = await b.central.connect()
        hello = await peer.read_value(chrs[HELLO_UUID])
        check(hello == b'hello from QEMU', f'hello read {hello!r}')
        return 'GATT read over UART2'


TESTS = {
    'advertise': t_advertise,
    'gatt': t_gatt,
    'pairing': t_pairing,
    'reconnect': t_reconnect,
    'reboot': t_reboot,
    'uart2': t_uart2,
}

# The ESP32-C3 has two UARTs
SKIP = {('esp32c3', 'uart2')}


class Context:
    pass


async def run(args):
    ctx = Context()
    ctx.qemu_dir = args.qemu_dir.resolve()
    ctx.icount = args.icount
    work = args.work.resolve()
    ctx.builder = Builder(work, args.rebuild)

    results = []
    for idf in args.idf:
        idf = idf.expanduser().resolve()
        tag = idf_tag(idf)
        for chip in args.chips.split(','):
            for name in args.tests.split(','):
                if (chip, name) in SKIP:
                    continue
                ctx.run_dir = work / 'runs' / tag / chip / name
                if ctx.run_dir.exists():
                    shutil.rmtree(ctx.run_dir)
                ctx.run_dir.mkdir(parents=True)
                t0 = time.monotonic()
                try:
                    detail = await asyncio.wait_for(
                        TESTS[name](ctx, idf, chip), args.timeout)
                    ok = True
                except Fail as e:
                    ok, detail = False, str(e)
                except asyncio.TimeoutError:
                    ok, detail = False, f'timed out after {args.timeout}s'
                except Exception as e:
                    ok, detail = False, f'error: {type(e).__name__}: {e}'
                status = 'PASS' if ok else 'FAIL'
                print(f'{status} {tag:8} {chip:8} {name:10} '
                      f'{time.monotonic() - t0:5.0f}s  {detail}', flush=True)
                results.append((tag, chip, name, ok, detail))
    return results


def write_report(path, results, args):
    tags = sorted({t for t, *_ in results}, reverse=True)
    chips = args.chips.split(',')
    names = args.tests.split(',')
    cells = {(t, c, n): ok for t, c, n, ok, _ in results}
    out = ['# Bluetooth LE test results', '',
           f'QEMU: `{args.qemu_dir}`' + (' with -icount' if args.icount
                                         else ''), '']
    header = ['Test'] + [f'{t} {c}' for t in tags for c in chips]
    out.append('| ' + ' | '.join(header) + ' |')
    out.append('|' + '---|' * len(header))
    for n in names:
        row = [n]
        for t in tags:
            for c in chips:
                ok = cells.get((t, c, n))
                row.append('n/a' if ok is None else ('PASS' if ok else 'FAIL'))
        out.append('| ' + ' | '.join(row) + ' |')
    out += ['', '## Details', '']
    for t, c, n, ok, detail in results:
        out.append(f'- **{t} {c} {n}**: {"PASS" if ok else "FAIL"}, {detail}')
    path.write_text('\n'.join(out) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf', action='append', required=True, type=Path,
                   help='ESP-IDF checkout (repeat for several versions)')
    p.add_argument('--chips', default='esp32,esp32c3,esp32s3')
    p.add_argument('--tests', default=','.join(TESTS))
    p.add_argument('--qemu-dir', type=Path,
                   default=HERE.parent.parent / 'build')
    p.add_argument('--work', type=Path, default=Path('esp_bt_work'))
    p.add_argument('--report', type=Path)
    p.add_argument('--rebuild', action='store_true')
    p.add_argument('--icount', action='store_true',
                   help='run QEMU with -icount shift=auto')
    p.add_argument('--timeout', type=int, default=600,
                   help='seconds per test, including the build')
    args = p.parse_args()

    results = asyncio.run(run(args))
    if args.report:
        write_report(args.report, results, args)
    sys.exit(0 if all(ok for *_, ok, _ in results) else 1)


if __name__ == '__main__':
    main()
