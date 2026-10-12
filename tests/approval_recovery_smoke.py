"""Offline real-process approval, durable execution and cancellation regressions.
Only temporary fixture files are written. No cloud API/key is used.
"""
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from smoke import Core, k
from desktop_memory_smoke import Provider, plan, stage, stored, tool_pairs


def active(directory):
    library = stored(directory)
    return next(s for s in library['sessions'] if s['id'] == library['active'])


def until(core, kind):
    events = []
    while True:
        event = core.read()
        events.append(event)
        assert event['type'] != 'done' or kind == 'done', events
        if event['type'] == kind:
            return event, events


def decide(core, event, allow=True, request_id=None):
    request = json.loads(event['text'])
    core.send(dict(type='permission', id=request['id'] if request_id is None else request_id, allow=allow))
    return request


def calls(*commands):
    return plan(tool=[dict(id='call_%d' % i, **(command if isinstance(command, dict) else dict(command=command)))
                      for i, command in enumerate(commands)])


def assert_pairs(directory):
    messages = active(directory)['messages']
    tool_pairs(messages)
    assert messages[-1]['role'] == 'assistant', messages
    return messages


def permissions(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    try:
        # Merely receiving an LLM command cannot cause any execution.
        provider.queue(calls(dict(command='echo approved>>approved.txt', stdin='complete input\n')), plan('done'))
        core.send(dict(type='chat', text='approve once', stream=True))
        event, _ = until(core, 'permission')
        request = json.loads(event['text'])
        assert Path(request['cwd']).samefile(directory), (request['cwd'], str(directory))
        assert request['command'] == 'echo approved>>approved.txt' and request['stdin'] == 'complete input\n'
        assert not (directory / 'approved.txt').exists()
        assert active(directory)['turn']['tools'][-1]['state'] == 'pending'
        prompt = provider.requests[-1]['messages'][0]['content']
        for expected in ('Windows XP SP3 x86', 'cmd.exe /D /C', 'sequentially', 'PowerShell', 'Reported host Windows version:', 'Command/input encoding:', 'not detected beside AgentCore.exe'):
            assert expected in prompt, (expected, prompt)
        assert 'in parallel' not in prompt
        decide(core, event)
        until(core, 'done')
        assert (directory / 'approved.txt').read_text().strip() == 'approved'
        journal = active(directory)['turn']['tools'][-1]
        assert journal['state'] == 'completed' and '[exit 0]' in journal['result']
        tool_pairs(provider.requests[-1]['messages'])

        # Denial and stale approvals are tool results, never launches.
        for bad_id in (None, request['id']):
            provider.queue(calls('echo forbidden>>denied.txt'), plan('denied acknowledged'))
            core.send(dict(type='chat', text='deny this', stream=False))
            event, _ = until(core, 'permission')
            decide(core, event, allow=bad_id is not None, request_id=bad_id)
            until(core, 'done')
            assert not (directory / 'denied.txt').exists()
            assert active(directory)['turn']['tools'][-1]['state'] == 'denied'
            assert 'Not executed' in provider.requests[-1]['messages'][-1]['content']

        # Complete first command, stop while approving second command in same batch.
        provider.queue(calls('echo first>>batch.txt', 'echo second>>batch.txt'))
        core.send(dict(type='chat', text='batch cancellation', stream=True))
        event, _ = until(core, 'permission')
        decide(core, event)
        event, _ = until(core, 'permission')
        assert (directory / 'batch.txt').read_text().strip() == 'first'
        assert k.SetEvent(core.cancel)
        until(core, 'done')
        messages = assert_pairs(directory)
        results = [m['content'] for m in messages if m['role'] == 'tool']
        assert '[exit 0]' in results[-2] and 'Not executed' in results[-1], results
        assert (directory / 'batch.txt').read_text().strip() == 'first'
    finally:
        core.close()
    # Restart does not spend an API call or re-execute either command.
    count = len(provider.requests)
    core = Core(directory)
    try:
        core.turn(dict(type='library'))
        assert len(provider.requests) == count
        assert (directory / 'batch.txt').read_text().strip() == 'first'
        provider.queue(plan('continued without replay'))
        core.turn(dict(type='chat', text='continue'))
        assert 'batch cancellation' in json.dumps(provider.requests[-1]['messages'])
        assert (directory / 'batch.txt').read_text().strip() == 'first'
    finally:
        core.close()


def failure_and_crash(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    try:
        provider.queue(calls('echo once>>once.txt'), plan(status=500))
        core.send(dict(type='chat', text='execute then API failure', stream=True))
        event, _ = until(core, 'permission')
        decide(core, event)
        _, events = until(core, 'done')
        assert any(e['type'] == 'error' for e in events)
        messages = assert_pairs(directory)
        assert 'execute then API failure' in json.dumps(messages)
        assert '[exit 0]' in [m['content'] for m in messages if m['role'] == 'tool'][-1]
        assert (directory / 'once.txt').read_text().strip() == 'once'
        # Crash after a second completed command, during the follow-up SSE response.
        gate = threading.Event()
        provider.queue(calls('echo durable-result & echo crash-once>>once.txt'), plan('incomplete answer', gate=gate))
        core.send(dict(type='chat', text='crash after command', stream=True))
        event, _ = until(core, 'permission')
        decide(core, event)
        result_event, _ = until(core, 'tool_result')
        saved_result = result_event['text']
        assert 'durable-result' in saved_result and saved_result in active(directory)['transcript'], 'Result display must share the durable checkpoint'
        until(core, 'reasoning_delta')
        assert active(directory)['pending']
        core.process.kill()
        core.process.wait(3)
        gate.set()
    finally:
        core.close()
    count = len(provider.requests)
    core = Core(directory)
    try:
        core.turn(dict(type='library'))
        assert not active(directory)['pending']
        assert len(provider.requests) == count
        messages = assert_pairs(directory)
        assert 'crash after command' in json.dumps(messages)
        assert len([m for m in messages if m['role'] == 'tool']) == 2
        replay = core.turn(dict(type='open', id=active(directory)['id']))
        assert saved_result in ''.join(e.get('text', '') for e in replay if e['type'] == 'replay')
        assert (directory / 'once.txt').read_text().splitlines() == ['once', 'crash-once']
    finally:
        core.close()


def pending_crash(source, directory, provider):
    stage(source, directory, provider)
    # Detection promises file presence only; the fixture is never executed.
    (directory / 'busybox.exe').write_bytes(b'presence-only fixture')
    core = Core(directory)
    try:
        provider.queue(calls('echo never>unapproved.txt'))
        core.send(dict(type='chat', text='crash before approval', stream=True))
        event, _ = until(core, 'permission')
        environment = provider.requests[-1]['messages'][0]['content']
        assert 'busybox.exe: "' in environment and 'busybox.exe: not detected' not in environment
        old_id = json.loads(event['text'])['id']
        core.process.kill()
        core.process.wait(3)
    finally:
        core.close()

    core = Core(directory)
    try:
        core.turn(dict(type='library'))
        journal = active(directory)['turn']['tools'][-1]
        assert journal['state'] == 'not_executed' and 'Not executed' in journal['result']
        assert not (directory / 'unapproved.txt').exists()
        assert_pairs(directory)
        provider.queue(calls('echo still-never>unapproved.txt'), plan('stale decision rejected'))
        core.send(dict(type='chat', text='a new task', stream=True))
        event, _ = until(core, 'permission')
        assert json.loads(event['text'])['id'] != old_id
        decide(core, event, request_id=old_id)
        until(core, 'done')
        assert not (directory / 'unapproved.txt').exists()
    finally:
        core.close()

    # A malformed journal must not be silently repaired/overwritten at startup.
    library = stored(directory)
    library['sessions'][0]['turn']['tools'][0]['state'] = 'invalid-state'
    path = directory / 'data' / 'library.json'
    path.write_text(json.dumps(library), encoding='utf-8')
    damaged = path.read_bytes()
    core = Core(directory)
    try:
        events = core.turn(dict(type='library'))
        assert any(e['type'] == 'error' for e in events), events
        assert path.read_bytes() == damaged
    finally:
        core.close()


def full_transcript_recovery(source, directory, provider, oversized=False):
    stage(source, directory, provider)
    core = Core(directory)
    try:
        core.turn(dict(type='library'))
    finally:
        core.close()
    library = stored(directory)
    session = library['sessions'][0]
    other = json.loads(json.dumps(session))
    other.update(id='other-session', title='Other session', transcript='Other session remains available')
    library['sessions'].append(other)
    # Exercise both the normal 256-byte reserve and an already full display.
    size = 4 * 1024 * 1024 - (0 if oversized else 256)
    session['transcript'] = 'a' * (size % 3) + '\u754c' * (size // 3)
    commands = [dict(id='recover_%d' % i, type='function', function=dict(name='cmd', arguments='{}')) for i in range(16)]
    session['pending'] = True
    session['turn'] = dict(messages=[dict(role='user', content='recover a full display'),
                                     dict(role='assistant', tool_calls=commands)], tools=[])
    result = '\u754c' * (4 * 1024 * 1024 // 3 + 1) + '\r\n[exit 0]' if oversized else 'complete result\r\n[exit 0]'
    session['turn']['tools'].append(dict(id='recover_0', request='recover-approval', index=1, state='completed', result=result))
    path = directory / 'data' / 'library.json'
    path.write_text(json.dumps(library, ensure_ascii=False), encoding='utf-8')
    count = len(provider.requests)
    core = Core(directory)
    try:
        events = core.turn(dict(type='library'))
        assert not any(e['type'] == 'error' for e in events), events
        recovered = active(directory)
        assert not recovered['pending']
        messages = assert_pairs(directory)
        results = [m['content'] for m in messages if m['role'] == 'tool']
        assert len(results) == 16 and results[0] == result
        assert all('Not executed' in text for text in results[1:])
        assert len(recovered['transcript'].encode('utf-8')) <= 4 * 1024 * 1024
        assert '[Display truncated;' in recovered['transcript']
        replay = core.turn(dict(type='open', id='other-session'))
        assert any('Other session remains available' in e.get('text', '') for e in replay)
        assert len(provider.requests) == count, 'Recovery must not call the model'
    finally:
        core.close()
    # The recovered library must remain valid on the following restart too.
    core = Core(directory)
    try:
        assert not any(e['type'] == 'error' for e in core.turn(dict(type='library')))
    finally:
        core.close()


def cancel_http(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    gate = threading.Event()
    try:
        provider.queue(calls('echo retained>http-stop.txt'), plan('unfinished', gate=gate))
        core.send(dict(type='chat', text='stop during follow-up HTTP', stream=True))
        event, _ = until(core, 'permission')
        decide(core, event)
        until(core, 'tool_result')
        until(core, 'reasoning_delta')
        started = time.monotonic()
        assert k.SetEvent(core.cancel)
        until(core, 'done')
        assert time.monotonic() - started < 3
        messages = assert_pairs(directory)
        assert '[exit 0]' in messages[-2]['content']
        assert active(directory)['turn']['tools'][-1]['state'] == 'completed'
    finally:
        gate.set()
        core.close()


def stop_and_unknown(source, directory, provider):
    stage(source, directory, provider)
    for crash in (False, True):
        core = Core(directory)
        marker = 'crash-running.txt' if crash else 'stop-running.txt'
        later = 'crash-later.txt' if crash else 'stop-later.txt'
        # A local child holds stdin open without reading; old synchronous stdin
        # writing could deadlock Stop. Job termination must kill this child too.
        command = 'echo started>%s & ping -n 30 127.0.0.1 >nul & echo repeated>%s' % (marker, later)
        provider.queue(calls(dict(command=command, stdin='x' * 100000)))
        try:
            core.send(dict(type='chat', text='interrupt running command', stream=True))
            event, _ = until(core, 'permission')
            decide(core, event)
            deadline = time.monotonic() + 4
            while not (directory / marker).exists() and time.monotonic() < deadline:
                time.sleep(.01)
            assert (directory / marker).exists()
            assert active(directory)['turn']['tools'][-1]['state'] == 'running'
            started = time.monotonic()
            if crash:
                core.process.kill()
                core.process.wait(3)
            else:
                assert k.SetEvent(core.cancel)
                until(core, 'done')
                assert time.monotonic() - started < 3
                journal = active(directory)['turn']['tools'][-1]
                assert journal['state'] == 'interrupted' and '[exit 130]' in journal['result'], journal
        finally:
            core.close()
        core = Core(directory)
        try:
            _, events = until_after_library(core)
            messages = assert_pairs(directory)
            journal = active(directory)['turn']['tools'][-1]
            assert journal['state'] == ('unknown' if crash else 'interrupted'), journal
            if crash:
                assert 'UNKNOWN' in messages[-2]['content']
                assert any('UNKNOWN' in e.get('text', '') for e in core.turn(dict(type='open', id=active(directory)['id']))), events
            assert not (directory / later).exists(), 'A child process survived termination'
            count = len(provider.requests)
            core.turn(dict(type='library'))
            assert len(provider.requests) == count, 'Recovery issued an unsolicited API request'
        finally:
            core.close()


def until_after_library(core):
    core.send(dict(type='library'))
    return until(core, 'done')


def save_failure(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    try:
        provider.queue(calls('echo forbidden>never.txt'))
        core.send(dict(type='chat', text='fail before launch', stream=True))
        event, _ = until(core, 'permission')
        previous = (directory / 'data' / 'library.json').read_bytes()
        # Prevent the next atomic save, after approval but before process launch.
        (directory / 'data' / 'library.tmp').mkdir()
        decide(core, event)
        _, events = until(core, 'done')
        assert any(e['type'] == 'error' for e in events), events
        assert not (directory / 'never.txt').exists()
        assert (directory / 'data' / 'library.json').read_bytes() == previous
    finally:
        core.close()


def main():
    source = Path(sys.argv[1]).resolve()
    provider = Provider()
    try:
        with tempfile.TemporaryDirectory(prefix='xp-approval-') as temporary:
            root = Path(temporary)
            permissions(source, root / 'permissions', provider)
            failure_and_crash(source, root / 'failure', provider)
            pending_crash(source, root / 'pending', provider)
            full_transcript_recovery(source, root / 'full', provider)
            full_transcript_recovery(source, root / 'oversized', provider, oversized=True)
            cancel_http(source, root / 'http', provider)
            stop_and_unknown(source, root / 'stop', provider)
            save_failure(source, root / 'save', provider)
    finally:
        provider.close()
    print('approval_recovery_smoke: XP environment, allow/deny/stale decisions, batch Stop, API failure, atomic result display, full-display recovery, unknown outcomes, child/stdin cancellation and failed-save refusal passed')


if __name__ == '__main__':
    main()
