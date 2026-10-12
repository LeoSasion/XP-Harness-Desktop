"""Offline desktop protocol, persistence and context regression checks.
Usage: python tests/desktop_memory_smoke.py build/host/AgentCore.exe
All files live in temporary folders; the only executed tool is harmless echo.
"""
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from smoke import Core


def packet(events, kind):
    matches = [event for event in events if event.get('type') == kind]
    assert matches, (kind, events)
    return json.loads(matches[-1]['text'])


def clean(events):
    assert not [event for event in events if event.get('type') == 'error'], events
    assert events[-1]['type'] == 'done', events
    return events


def refused_start(core, error):
    """A startup refusal can close the pipe before the first client write."""
    if 'exited during startup' in str(error).lower() or 'pipe closed' in str(error).lower():
        return True
    if core is not None and not str(error):
        try:
            core.process.wait(timeout=1)
            return True
        except subprocess.TimeoutExpired:
            pass
    return False


def tool_pairs(messages):
    """Every tool reply must follow the complete assistant call set in order."""
    waiting = []
    for message in messages:
        if message['role'] == 'tool':
            assert waiting and message['tool_call_id'] == waiting.pop(0), messages
        else:
            assert not waiting, messages
            waiting = [call['id'] for call in message.get('tool_calls', [])]
    assert not waiting, messages


def plan(answer='answer', reasoning='public reasoning', *, tool=False, status=200, gate=None):
    return dict(answer=answer, reasoning=reasoning, tool=tool, status=status,
                gate=gate, finished=threading.Event())


class Provider:
    def __init__(self):
        self.plans = deque()
        self.requests = []
        self.failures = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                response = None
                try:
                    body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    owner.requests.append(body)
                    assert self.headers.get('Authorization') is None, 'Tests must not use a key'
                    assert owner.plans, 'Unexpected API request'
                    response = owner.plans.popleft()
                    if response['status'] != 200:
                        data = b'{"error":{"message":"offline test failure"}}'
                        self.send_response(response['status'])
                        self.send_header('Content-Length', str(len(data)))
                        self.end_headers()
                        self.wfile.write(data)
                        return
                    message = {'role': 'assistant', 'content': response['answer'],
                               'reasoning_content': response['reasoning']}
                    finish = 'stop'
                    if response['tool']:
                        finish = 'tool_calls'
                        commands = [dict(id='call_echo', command='echo cwd=%CD%', stdin='safe input\n')] if response['tool'] is True else response['tool']
                        message.update(content=None, tool_calls=[{
                            'id': command.get('id', 'call_%d' % i), 'type': 'function', 'function': {
                                'name': 'cmd', 'arguments': json.dumps({key: value for key, value in command.items() if key != 'id'})}}
                            for i, command in enumerate(commands)])
                    if body.get('stream'):
                        self.send_response(200)
                        self.send_header('Content-Type', 'text/event-stream')
                        self.end_headers()

                        def chunk(delta, reason=None):
                            data = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': reason}]}
                            self.wfile.write(('data: ' + json.dumps(data) + '\n\n').encode())
                            self.wfile.flush()

                        chunk({'role': 'assistant', 'reasoning_content': response['reasoning']})
                        if response['gate'] is not None:
                            assert response['gate'].wait(10), 'Test did not release streaming gate'
                        if response['tool']:
                            calls = message['tool_calls']
                            for i, call in enumerate(calls):
                                call['index'] = i
                            chunk({'tool_calls': calls})
                        else:
                            half = max(1, len(response['answer']) // 2)
                            chunk({'content': response['answer'][:half]})
                            chunk({'content': response['answer'][half:]})
                        chunk({}, finish)
                        self.wfile.write(b'data: [DONE]\n\n')
                    else:
                        data = json.dumps({'choices': [{'message': message, 'finish_reason': finish}]}).encode()
                        self.send_response(200)
                        self.send_header('Content-Type', 'application/json')
                        self.send_header('Content-Length', str(len(data)))
                        self.end_headers()
                        self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass  # Expected when the owned core is stopped mid-stream.
                except Exception as exc:
                    owner.failures.append(repr(exc))
                finally:
                    if response is not None:
                        response['finished'].set()

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def queue(self, *responses):
        assert not self.plans, 'Previous turn left an unused server response'
        self.plans.extend(responses)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(3)
        assert not self.failures, self.failures
        assert not self.plans, 'Some expected model requests were not made'


def stage(source, directory, provider):
    directory.mkdir(parents=True, exist_ok=True)
    for name in ('AgentCore.exe', 'libcurl-4.dll', 'curl-ca-bundle.crt'):
        shutil.copy2(source.parent / name, directory / name)
    (directory / 'vc6ai.ini').write_text(
        '[LLM]\nApiUrl=http://127.0.0.1:%d/chat/completions\n'
        'ApiKey=\nModel=offline-memory-test\nStream=1\nContextBytes=16384\n'
        % provider.server.server_port, encoding='ascii')


def stored(directory):
    return json.loads((directory / 'data' / 'library.json').read_text(encoding='utf-8'))


def session_data(directory, session_id):
    return next(item for item in stored(directory)['sessions'] if item['id'] == session_id)


def open_session(core, session_id):
    events = clean(core.turn({'type': 'open', 'id': session_id}))
    session = packet(events, 'session')
    assert session['id'] == session_id, session
    assert any(event['type'] == 'clear' for event in events), events
    return session, '\n'.join(event.get('text', '') for event in events if event['type'] == 'replay')


def text_turn(core, provider, session_id, text, answer, stream=False):
    provider.queue(plan(answer))
    events = clean(core.turn({'type': 'chat', 'id': session_id, 'text': text, 'stream': stream}))
    if stream:
        assert ''.join(event.get('text', '') for event in events if event['type'] == 'answer_delta') == answer, events
    else:
        assert [event.get('text') for event in events if event['type'] == 'assistant'] == [answer], events
    return events


def check_library_and_stream(source, directory, provider):
    stage(source, directory, provider)
    project_a = directory / 'workspace-a'
    project_b = directory / 'workspace-b'
    project_a.mkdir()
    project_b.mkdir()
    (project_a / 'AGENTS.md').write_text('PROJECT_A_AGENTS_MARKER', encoding='ascii')
    (project_b / 'AGENTS.md').write_text('PROJECT_B_AGENTS_MARKER', encoding='ascii')
    (project_a / 'alpha-only.txt').write_text('offline fixture', encoding='ascii')
    (project_b / 'beta-only.txt').write_text('offline fixture', encoding='ascii')
    core = Core(directory)
    first = sibling = second = None
    try:
        initial = packet(clean(core.turn({'type': 'library'})), 'catalog')
        assert len(initial['projects']) == len(initial['sessions']) == 1, initial
        assert not provider.requests, 'Opening the library must remain offline'
        events = clean(core.turn({'type': 'new_project', 'text': '项目 Alpha', 'path': str(project_a)}))
        first = packet(events, 'session')
        assert Path(first['path']) == project_a, first
        assert first['context_bytes'] == 16384, first
        first_id = first['id']
        clean(core.turn({'type': 'rename', 'id': first_id, 'text': 'Alpha renamed'}))
        notes = 'PROJECT_ALPHA_SHARED_NOTE_42'
        clean(core.turn({'type': 'memory', 'text': notes}))
        text_turn(core, provider, first_id, 'ALPHA_PRIVATE_TURN', 'ALPHA_PRIVATE_REPLY')
        outgoing = json.dumps(provider.requests[-1]['messages'])
        assert notes in outgoing and 'PROJECT_A_AGENTS_MARKER' in outgoing and 'alpha-only.txt' in outgoing, outgoing
        assert 'PROJECT_B_AGENTS_MARKER' not in outgoing and 'beta-only.txt' not in outgoing, outgoing
        sibling = packet(clean(core.turn({'type': 'new_chat', 'project': first['project'], 'text': 'Alpha sibling'})), 'session')
        text_turn(core, provider, sibling['id'], 'SIBLING_PRIVATE_TURN', 'SIBLING_REPLY')
        outgoing = json.dumps(provider.requests[-1]['messages'])
        assert notes in outgoing and 'ALPHA_PRIVATE_TURN' not in outgoing, outgoing
        second = packet(clean(core.turn({'type': 'new_project', 'text': 'Project Beta', 'path': str(project_b)})), 'session')
        text_turn(core, provider, second['id'], 'BETA_PRIVATE_TURN', 'BETA_REPLY')
        outgoing = json.dumps(provider.requests[-1]['messages'])
        assert notes not in outgoing and 'ALPHA_PRIVATE_TURN' not in outgoing and 'SIBLING_PRIVATE_TURN' not in outgoing
        assert 'PROJECT_B_AGENTS_MARKER' in outgoing and 'beta-only.txt' in outgoing, outgoing
        assert 'PROJECT_A_AGENTS_MARKER' not in outgoing and 'alpha-only.txt' not in outgoing, outgoing
        reopened, replay = open_session(core, first_id)
        assert reopened['title'] == 'Alpha renamed' and reopened['memory'] == notes, reopened
        assert 'ALPHA_PRIVATE_TURN' in replay and 'ALPHA_PRIVATE_REPLY' in replay, replay
        assert 'SIBLING_PRIVATE_TURN' not in replay and 'BETA_PRIVATE_TURN' not in replay, replay

        gate = threading.Event()
        early = plan('streamed answer', gate=gate)
        provider.queue(early)
        core.send({'type': 'chat', 'id': first_id, 'text': 'early stream', 'stream': True})
        events = []
        try:
            while not any(event['type'] == 'reasoning_delta' for event in events):
                events.append(core.read())
                assert events[-1]['type'] != 'done', events
            assert not early['finished'].is_set(), 'Delta arrived only after the response completed'
        finally:
            gate.set()
        while events[-1]['type'] != 'done':
            events.append(core.read())
        clean(events)
        assert ''.join(e.get('text', '') for e in events if e['type'] == 'answer_delta') == 'streamed answer'

        provider.queue(plan(tool=True), plan('tool completed'))
        events = clean(core.turn({'type': 'chat', 'id': first_id, 'text': 'show cwd', 'stream': True}))
        assert any(e['type'] == 'tool_stdin' and 'safe input' in e.get('text', '') for e in events), events
        outputs = [e.get('text', '') for e in events if e['type'] == 'tool_result']
        assert any(str(project_a).lower() in output.lower() and '[exit 0]' in output for output in outputs), outputs
        messages = provider.requests[-1]['messages']
        tool_pairs(messages)
        calls = [m for m in messages if m.get('tool_calls')]
        assert calls[-1].get('reasoning_content') == 'public reasoning', calls[-1]
        before_failure = session_data(directory, first_id)['messages']
        provider.queue(plan(tool=True), plan(status=500))
        events = core.turn({'type': 'chat', 'id': first_id, 'text': 'failed turn', 'stream': True})
        assert any(e['type'] == 'error' for e in events), events
        failed_history = session_data(directory, first_id)['messages']
        assert failed_history[:len(before_failure)] == before_failure
        assert any(m['role'] == 'tool' and 'cwd=' in m['content'] for m in failed_history[len(before_failure):]), 'Executed tool result was lost'
        tool_pairs(session_data(directory, first_id)['messages'])

        # A second process may connect long enough to report the lock error, but
        # must never obtain a writable library or silently create a replacement.
        before_lock = (directory / 'data' / 'library.json').read_bytes()
        second_core = None
        try:
            try:
                second_core = Core(directory)
                locked = second_core.turn({'type': 'library'})
                assert any(e['type'] == 'error' for e in locked), locked
            except AssertionError as exc:
                assert refused_start(second_core, exc), exc
        finally:
            if second_core:
                second_core.close()
        assert (directory / 'data' / 'library.json').read_bytes() == before_lock

        # Stop mirrors Desktop's owned-process termination, mid-response.
        gate = threading.Event()
        interrupted = plan('must never become completed history', gate=gate)
        provider.queue(plan(tool=True), interrupted)
        core.send({'type': 'chat', 'id': first_id, 'text': 'STOP_UNCOMMITTED_TURN', 'stream': True})
        saw_tool_result = False
        try:
            while True:
                event = core.read()
                if event['type'] == 'permission':
                    permission = json.loads(event['text'])
                    core.send({'type': 'permission', 'id': permission['id'], 'allow': True})
                assert event['type'] != 'done', event
                if event['type'] == 'tool_result':
                    saw_tool_result = True
                if saw_tool_result and event['type'] == 'reasoning_delta':
                    break
            core.process.kill()
            core.process.wait(3)
        finally:
            gate.set()
        core.close()
        core = Core(directory)
        resumed, replay = open_session(core, first_id)
        assert resumed['title'] == 'Alpha renamed' and resumed['memory'] == notes
        assert 'ALPHA_PRIVATE_REPLY' in replay and 'tool completed' in replay, replay
        recovered = session_data(directory, first_id)['messages']
        assert recovered[:len(failed_history)] == failed_history, 'Recovery changed prior history'
        assert 'STOP_UNCOMMITTED_TURN' in json.dumps(recovered)
        assert any(m['role'] == 'tool' for m in recovered[len(failed_history):]), 'Stop discarded executed tool results'
        tool_pairs(recovered)

        # Exceed the context budget while retaining complete raw history on disk.
        for i in range(7):
            prompt = 'LONG_USER_%d ' % i + ('u%d' % i) * 1200
            answer = 'LONG_ANSWER_%d ' % i + ('a%d' % i) * 1200
            text_turn(core, provider, first_id, prompt, answer)
            messages = provider.requests[-1]['messages']
            projected = json.dumps(messages, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
            assert len(projected) <= 16384, len(projected)
            assert messages[-1]['content'] == prompt
            tool_pairs(messages)
            if i == 0:
                assert b'ALPHA_PRIVATE_REPLY' in projected and b'tool completed' in projected
                assert any(m.get('tool_calls') and m.get('reasoning_content') == 'public reasoning'
                           for m in messages), 'Restart did not restore the prior tool reasoning chain'
        raw = session_data(directory, first_id)['messages']
        assert len(json.dumps(raw).encode()) > 16384
        assert raw[:len(before_failure)] == before_failure, 'Context projection changed raw completed history'
        for i in range(7):
            assert any(m.get('content', '').startswith('LONG_USER_%d ' % i) for m in raw if isinstance(m.get('content'), str))
            assert any(m.get('content', '').startswith('LONG_ANSWER_%d ' % i) for m in raw if isinstance(m.get('content'), str))
        tool_pairs(raw)
        core.close()
        core = Core(directory)
        _, replay = open_session(core, first_id)
        assert 'LONG_USER_0' in replay and 'LONG_ANSWER_6' in replay
        assert session_data(directory, first_id)['messages'] == raw
        # A transcript may exceed one IPC frame; replay must preserve both ends.
        large_answer = 'LARGE_REPLY_START ' + 'x' * (270 * 1024) + ' LARGE_REPLY_END'
        text_turn(core, provider, first_id, 'large transcript fixture', large_answer, stream=True)
        core.close()
        core = Core(directory)
        _, replay = open_session(core, first_id)
        assert len(replay.encode('utf-8')) > 256 * 1024
        assert 'LARGE_REPLY_START' in replay and 'LARGE_REPLY_END' in replay
        assert session_data(directory, first_id)['messages'][-1]['content'] == large_answer
        catalog = packet(clean(core.turn({'type': 'library'})), 'catalog')
        assert any(p['name'] == '项目 Alpha' for p in catalog['projects']), catalog
        assert len(catalog['sessions']) == 4, catalog
    finally:
        core.close()


def check_corrupt_library(source, directory, provider):
    stage(source, directory, provider)
    (directory / 'data').mkdir()
    target = directory / 'data' / 'library.json'
    for broken in (b'{"version":1,"projects":[', b'{"version":1,"projects":[],"sessions":[],"active":"missing"}'):
        target.write_bytes(broken)
        core = None
        try:
            try:
                core = Core(directory)
                events = core.turn({'type': 'library'})
                assert any(e['type'] == 'error' for e in events), events
            except AssertionError as exc:
                assert refused_start(core, exc), exc
        finally:
            if core:
                core.close()
        assert target.read_bytes() == broken, 'Corrupt library was silently overwritten'


def check_save_failure(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    obstacle = directory / 'data' / 'library.tmp'
    try:
        catalog = packet(clean(core.turn({'type': 'library'})), 'catalog')
        session_id = catalog['active']
        before = (directory / 'data' / 'library.json').read_bytes()
        request_count = len(provider.requests)
        # A directory at the atomic-save destination reliably fails CreateFile
        # without depending on Administrator permissions or filesystem ACLs.
        obstacle.mkdir()
        events = core.turn({'type': 'chat', 'id': session_id, 'text': 'must not reach provider'})
        assert any(e['type'] == 'error' for e in events), events
        assert len(provider.requests) == request_count, 'A turn ran without durable pending state'
        assert (directory / 'data' / 'library.json').read_bytes() == before
        core.close()
        obstacle.rmdir()
        core = Core(directory)
        open_session(core, session_id)
        assert session_data(directory, session_id)['messages'] == []
        text_turn(core, provider, session_id, 'recovered save', 'save recovered')
    finally:
        core.close()
        if obstacle.is_dir():
            obstacle.rmdir()


def main():
    source = Path(sys.argv[1]).resolve()
    provider = Provider()
    try:
        with tempfile.TemporaryDirectory(prefix='vc6ai-desktop-memory-') as tmp:
            directory = Path(tmp)
            check_library_and_stream(source, directory / 'app', provider)
            check_corrupt_library(source, directory / 'corrupt', provider)
            check_save_failure(source, directory / 'save-failure', provider)
    finally:
        provider.close()
    print('desktop_memory_smoke: project/session isolation, memory, streaming, tool cwd, '
          'restart/Stop recovery, retained failed-tool history, context budget, raw history, '
          'lock/corruption and atomic-save failure checks passed')


if __name__ == '__main__':
    main()
