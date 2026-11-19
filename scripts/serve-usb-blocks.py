#!/usr/bin/env python3
"""Serve the four pinned OPD2413 USB boot inputs through Windows USBMux."""

import argparse
from collections import Counter
from contextlib import ExitStack
import csv
import os
from pathlib import Path
import plistlib
import socket
import struct
import time


MAX_BLOCK = 1024 * 1024
USB_MUX = ('127.0.0.1', 27015)
INPUTS = (
    ('OS.raw', 6077580800),
    ('SystemOS.dmg', 3303014400),
    ('OS.manifest.bin', 47481664),
    ('SystemOS.manifest.bin', 25805344),
)


def recv_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError('device closed the connection')
        data.extend(chunk)
    return bytes(data)


def send_request(stream, message):
    payload = plistlib.dumps(message)
    stream.sendall(struct.pack('<4I', len(payload) + 16, 1, 8, 1) + payload)


def read_reply(stream):
    size, _, _, _ = struct.unpack('<4I', recv_exact(stream, 16))
    if not 16 < size <= 1024 * 1024:
        raise ValueError(f'invalid USBMux reply size {size}')
    return plistlib.loads(recv_exact(stream, size - 16))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images-dir', type=Path, required=True)
    parser.add_argument('--port', type=int, default=62078)
    parser.add_argument('--device-id', type=int)
    parser.add_argument('--trace', type=Path, help='write a new per-request CSV')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('--port must be between 1 and 65535')

    with ExitStack() as stack:
        volumes = {}
        for index, (name, expected) in enumerate(INPUTS):
            path = args.images_dir / name
            try:
                handle = stack.enter_context(path.open('rb'))
                size = os.fstat(handle.fileno()).st_size
            except OSError as error:
                parser.error(str(error))
            if size != expected:
                parser.error(f'{path} has {size} bytes, expected {expected}')
            volumes[index] = handle, 0, size

        with socket.create_connection(USB_MUX, timeout=5) as control:
            send_request(control, {'MessageType': 'ListDevices', 'ProgName': 'Erhai blocks',
                                   'ClientVersionString': '1.0', 'kLibUSBMuxVersion': 3})
            devices = [item['DeviceID'] for item in read_reply(control).get('DeviceList', [])
                       if item.get('Properties', {}).get('ConnectionType') in (None, 'USB')]
        if args.device_id is not None:
            devices = [device for device in devices if device == args.device_id]
        if len(devices) != 1:
            parser.exit(1, f'Expected one USBMux USB device, found {devices}\n')

        trace = None
        if args.trace:
            stream = stack.enter_context(args.trace.open('x', newline='', encoding='utf-8'))
            trace = csv.writer(stream)
            trace.writerow(('elapsed_ms', 'volume', 'offset', 'length', 'outcome'))

        deadline = time.monotonic() + 20
        while True:
            with socket.create_connection(USB_MUX, timeout=10) as client:
                send_request(client, {'MessageType': 'Connect', 'DeviceID': devices[0],
                                      'PortNumber': socket.htons(args.port), 'ProgName': 'Erhai blocks',
                                      'ClientVersionString': '1.0', 'kLibUSBMuxVersion': 3})
                result = read_reply(client).get('Number')
                if result == 0:
                    client.sendall(b'EBL2' + bytes(8))
                    ack = recv_exact(client, 4)
                    if ack != b'EBOK':
                        raise SystemExit(f'Device rejected the USB block role: {ack!r}')
                    client.settimeout(None)
                    print('Connected. Keep this process running while iPadOS uses the USB volumes.', flush=True)
                    serve_blocks(client, volumes, trace)
                    return
            if result != 3 or time.monotonic() >= deadline:
                raise SystemExit(f'Device refused the block port, Number={result}')
            time.sleep(1)


def serve_blocks(client, volumes, trace=None):
    served = 0
    total = 0
    counts = dict.fromkeys(volumes, 0)
    bytes_by_volume = dict.fromkeys(volumes, 0)
    lengths_by_volume = {volume: Counter() for volume in volumes}
    started = time.monotonic()
    phase = 'request header'
    request = None
    last_sent = None

    def record(outcome):
        if trace is not None and request is not None:
            elapsed_ms = round((time.monotonic() - started) * 1000)
            trace.writerow((elapsed_ms, *request, outcome))

    try:
        while True:
            phase = 'request header'
            request = None
            magic, volume, offset, length = struct.unpack('<4sIQI', recv_exact(client, 20))
            request = (volume, offset, length)
            if magic != b'EBK1':
                record('bad magic')
                print(f'bad request magic {magic!r}', flush=True)
                break
            entry = volumes.get(volume)
            if entry is None or not 0 < length <= MAX_BLOCK or offset + length > entry[2]:
                phase = 'send refusal'
                client.sendall(struct.pack('<4sII', b'EBK1', 22, 0))
                record('refused')
                print(f'refused vol={volume} off={offset} len={length}', flush=True)
                continue
            handle, base, _ = entry
            phase = 'read image'
            handle.seek(base + offset)
            data = handle.read(length)
            if len(data) != length:
                phase = 'send short-read error'
                client.sendall(struct.pack('<4sII', b'EBK1', 5, 0))
                record('short image read')
                print(f'short read vol={volume} off={offset}', flush=True)
                continue
            phase = 'send data'
            client.sendall(struct.pack('<4sII', b'EBK1', 0, length) + data)
            last_sent = request
            phase = 'record response'
            record('sent')
            served += 1
            total += length
            counts[volume] += 1
            bytes_by_volume[volume] += length
            lengths_by_volume[volume][length] += 1
            if counts[volume] <= 12:
                print(f'vol={volume} read={counts[volume]} off={offset} len={length}', flush=True)
            if served % 256 == 0:
                rate = total / (time.monotonic() - started) / 1048576
                print(f'{served} requests, {total / 1048576:.1f} MiB, {rate:.2f} MiB/s', flush=True)
    except (EOFError, ConnectionError) as error:
        record(f'{phase} failed')
        print(f'client ended during {phase}: {error}; request={request}; last sent={last_sent}', flush=True)
    finally:
        print(f'served {served} requests / {total:,} bytes; per volume {counts}', flush=True)
        if trace is not None:
            print(f'bytes per volume {bytes_by_volume}; request sizes per volume '
                  f'{ {volume: dict(sorted(sizes.items())) for volume, sizes in lengths_by_volume.items()} }',
                  flush=True)


if __name__ == '__main__':
    main()
