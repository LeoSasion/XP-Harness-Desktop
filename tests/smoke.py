"""Exercise the real AgentCore named pipe, offline, on a Windows host.
Usage: python tests/smoke.py build/host/AgentCore.exe
"""
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

k = C.WinDLL('kernel32', use_last_error=True)
k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.LPVOID, W.DWORD, W.DWORD, W.HANDLE]
k.CreateFileW.restype = W.HANDLE
k.CloseHandle.argtypes = [W.HANDLE]
k.ReadFile.argtypes = [W.HANDLE, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), W.LPVOID]
k.WriteFile.argtypes = k.ReadFile.argtypes
k.PeekNamedPipe.argtypes = [W.HANDLE, W.LPVOID, W.DWORD, W.LPVOID, C.POINTER(W.DWORD), W.LPVOID]
INVALID = C.c_void_p(-1).value


class Core:
    def __init__(self, directory):
        self.name = rf'\\.\pipe\vc6agent-{os.getpid()}-{time.monotonic_ns()}'
        self.process = subprocess.Popen([str(directory / 'AgentCore.exe'), '--pipe', self.name,
                                         '--parent', str(os.getpid())], cwd=directory,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.handle = INVALID
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.handle = k.CreateFileW(self.name, 0xC0000000, 0, None, 3, 0x00110000, None)
            if self.handle != INVALID:
                return
            if self.process.poll() is not None:
                raise AssertionError(f'Core exited during startup: {self.process.returncode}')
            time.sleep(.02)
        self.close()
        raise AssertionError('Core did not open its named pipe')

    def write(self, data):
        count = W.DWORD()
        buf = C.create_string_buffer(data)
        assert k.WriteFile(self.handle, buf, len(data), C.byref(count), None)
        assert count.value == len(data)

    def send(self, value, fragmented=False):
        raw = json.dumps(value, ensure_ascii=False).encode('utf-8')
        frame = struct.pack('<I', len(raw)) + raw
        for part in ([frame[:1], frame[1:3], frame[3:9], frame[9:]] if fragmented else [frame]):
            self.write(part)

    def exact(self, size):
        data = b''
        deadline = time.monotonic() + 5
        while len(data) < size:
            available = W.DWORD()
            if not k.PeekNamedPipe(self.handle, None, 0, None, C.byref(available), None):
                raise AssertionError('Pipe closed unexpectedly')
            if not available.value:
                assert time.monotonic() < deadline, 'Timed out reading a frame'
                time.sleep(.01)
                continue
            buf = C.create_string_buffer(min(size - len(data), available.value))
            count = W.DWORD()
            assert k.ReadFile(self.handle, buf, len(buf), C.byref(count), None)
            data += buf.raw[:count.value]
        return data

    def read(self):
        size, = struct.unpack('<I', self.exact(4))
        assert 0 < size <= 256 * 1024
        return json.loads(self.exact(size).decode('utf-8'))

    def turn(self, request, fragmented=False):
        self.send(request, fragmented)
        events = []
        while True:
            event = self.read()
            events.append(event)
            if event.get('type') == 'done':
                return events

    def close(self):
        if self.handle != INVALID:
            k.CloseHandle(self.handle)
            self.handle = INVALID
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def main():
    source = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='vc6ai-smoke-') as tmp:
        directory = Path(tmp)
        for name in ('AgentCore.exe', 'libcurl-4.dll', 'curl-ca-bundle.crt'):
            shutil.copy2(source.parent / name, directory / name)
        (directory / 'vc6ai.ini').write_text('[OpenRouter]\nApiKey=\nModel=offline-test\n', encoding='ascii')
        core = Core(directory)
        try:
            # Startup is deliberately local: no API credentials or network needed.
            events = core.turn({'type': 'reset'}, fragmented=True)
            assert not any(e['type'] == 'error' for e in events), events
            events = core.turn({'type': 'chat', 'text': '\u4f60\u597d\n"UTF-8"'})
            assert any(e['type'] == 'error' for e in events), events
            events = core.turn({'type': 'unknown'})
            assert any(e['type'] == 'error' for e in events), events
            core.send({'type': 'shutdown'})
            core.process.wait(timeout=3)
            assert core.process.returncode == 0
        finally:
            core.close()
        for payload in (b'{}garbage', b'{"type":"reset"}\x00ignored'):
            core = Core(directory)
            try:
                core.write(struct.pack('<I', len(payload)) + payload)
                if b'\x00' in payload:
                    core.process.wait(timeout=3)
                else:
                    events = []
                    while not events or events[-1]['type'] != 'done':
                        events.append(core.read())
                    assert any(e['type'] == 'error' for e in events), events
            finally:
                core.close()
        core = Core(directory)
        try:
            core.write(struct.pack('<I', 256 * 1024 + 1))
            core.process.wait(timeout=3)
        finally:
            core.close()
    print('smoke: fragmented IPC, Unicode, reset, missing key, unknown type, shutdown, malformed and oversized frames passed')


if __name__ == '__main__':
    main()
