"""CLI streaming integration test; local HTTP only, no credentials needed."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


def main():
    source = Path(sys.argv[1]).resolve()
    requests, output, scenario = [], bytearray(), ['normal']
    first_visible, failed = threading.Event(), []

    class Mock(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def event(self, delta=None, finish=None, raw=None):
            data = raw or json.dumps({'choices': [{'delta': delta or {}, 'finish_reason': finish}]},
                                    ensure_ascii=False)
            packet = ('data: ' + data + '\r\n\r\n').encode('utf8')
            # Split UTF-8 characters, JSON and line endings across network writes.
            for i in range(0, len(packet), 7):
                self.wfile.write(packet[i:i + 7])
                self.wfile.flush()

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            requests.append(body)
            bad = scenario[0] != 'normal' and body['messages'][-1]['content'] == 'bad'
            if bad and scenario[0] == 'http-error':
                self.send_response(401)
                self.end_headers()
                self.wfile.write(b'{"error":{"message":"mock unauthorized"}}')
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            self.wfile.write(b': keep-alive\n\n')
            if bad:
                self.event({'tool_calls': [{'index': 16 if scenario[0] == 'bad-index' else 0,
                            'id': 'must_not_run', 'function': {'name': 'cmd',
                            'arguments': '{"command":"echo MUST-NOT-RUN"}'}}]})
                self.event(finish='tool_calls')
                return  # disconnected without [DONE]: tools must never execute
            n = len(requests)
            if n == 1:
                self.event({'reasoning_content': '先检查工具。'})
                if not first_visible.wait(3):
                    failed.append('Thinking was buffered until response completion')
                self.event({'tool_calls': [{'index': 0, 'id': 'call_stream', 'type': 'function',
                                           'function': {'name': 'cmd', 'arguments': '{"command":"echo '}}]})
                self.event({'tool_calls': [{'index': 0, 'function': {'arguments': 'STREAM-OK"}'}}]})
                self.event(finish='tool_calls')
            else:
                self.event({'reasoning_content': '根据结果回答。'})
                self.event({'content': '中文'})
                self.event({'content': '回答'})
                self.event(finish='stop')
            self.event(raw='[DONE]')

    server = HTTPServer(('127.0.0.1', 0), Mock)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='xp-stream-test-') as tmp:
            directory = Path(tmp)
            for name in ('AgentCore.exe', 'libcurl-4.dll', 'curl-ca-bundle.crt'):
                shutil.copy2(source.parent / name, directory / name)
            (directory / 'vc6ai.ini').write_text(
                f'[LLM]\nApiUrl=http://127.0.0.1:{server.server_port}/chat/completions\n'
                'Model=stream-test\nStream=1\n'
                'ExtraBody={"thinking":{"type":"enabled"},"model":"must-not-override","stream":false}\n', encoding='ascii')
            process = subprocess.Popen([str(directory / 'AgentCore.exe')], cwd=directory,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def read_output():
                while True:
                    chunk = process.stdout.read1(4096)
                    if not chunk:
                        break
                    output.extend(chunk)
                    if '先检查工具。'.encode() in output:
                        first_visible.set()

            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            process.stdin.write(b'first turn\nfollow up\n/exit\n')
            process.stdin.close()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError('CLI stream test timed out')
            reader.join(timeout=3)
            stderr = process.stderr.read().decode('utf8', errors='replace')
            text = output.decode('utf8')
            assert process.returncode == 0 and not stderr, stderr
            assert first_visible.is_set() and not failed, failed
            assert len(requests) == 3, requests
            assert all(r['stream'] is True for r in requests)
            assert all(r['model'] == 'stream-test' and r['thinking'] == {'type': 'enabled'} for r in requests)
            assert '[Thinking]' in text and '[Answer]' in text, text
            assert text.count('中文回答') == 2, text  # no duplicated final output
            assert '[Tool result: call_stream]' in text and '[exit 0]' in text, text
            tools = [m for m in requests[1]['messages'] if m['role'] == 'tool']
            assert tools[0]['tool_call_id'] == 'call_stream' and 'STREAM-OK' in tools[0]['content'], tools
            assistants = [m for m in requests[2]['messages'] if m['role'] == 'assistant']
            assert [m['reasoning_content'] for m in assistants] == ['先检查工具。', '根据结果回答。'], assistants
            for case in ('truncated', 'bad-index', 'http-error'):
                scenario[0] = case
                requests.clear()
                result = subprocess.run([str(directory / 'AgentCore.exe')], cwd=directory,
                                        input=b'bad\nrecovery\n/exit\n', capture_output=True, timeout=15)
                assert result.returncode == 0 and result.stderr, (case, result.stderr)
                assert b'[Tool result:' not in result.stdout, (case, result.stdout)
                assert len(requests) == 2, (case, requests)
                history = requests[1]['messages']
                assert [m['role'] for m in history] == ['system', 'user'], (case, history)
                assert history[-1]['content'] == 'recovery', history
                assert '中文回答'.encode() in result.stdout, (case, result.stdout)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
    print('stream_smoke: live output, fragmented SSE/UTF-8, tool arguments/results, reasoning replay, truncated stream, invalid index, HTTP errors and recovery passed')


if __name__ == '__main__':
    main()
