"""Offline real-Core export/delete and actionable HTTP/connection-error regressions."""
import json
from pathlib import Path
import socket
import sys
import tempfile

from smoke import Core, k
from desktop_memory_smoke import Provider, clean, packet, plan, stage, stored, tool_pairs


def check_sessions(source, directory, provider):
    stage(source, directory, provider)
    workspace = directory / 'project'
    workspace.mkdir()
    marker = workspace / 'keep.txt'
    marker.write_text('project file stays', encoding='ascii')
    core = Core(directory)
    try:
        initial = packet(clean(core.turn(dict(type='library'))), 'catalog')
        root_id = initial['active']
        current = packet(clean(core.turn(dict(type='new_project', text='Export project', path=str(workspace)))), 'session')
        project, chat = current['project'], current['id']
        clean(core.turn(dict(type='memory', text='KEEP_SHARED_NOTE')))
        provider.queue(plan(tool=True), plan('export answer \u4e2d\u6587 ' + 'x' * (270 * 1024)))
        clean(core.turn(dict(type='chat', text='export full conversation', stream=True)))
        before = stored(directory)
        expected = next(s for s in before['sessions'] if s['id'] == chat)
        library_path = directory / 'data' / 'library.json'
        original = library_path.read_bytes()
        requests = len(provider.requests)
        destination = directory / '\u5bfc\u51fa\u4f1a\u8bdd.json'
        destination.write_text('old export', encoding='ascii')
        clean(core.turn(dict(type='export_chat', id=chat, path=str(destination))))
        exported = json.loads(destination.read_text(encoding='utf-8'))
        assert exported == expected
        tool_pairs(exported['messages'])
        assert any(m.get('reasoning_content') == 'public reasoning' for m in exported['messages'])
        assert len(destination.read_bytes()) > 256 * 1024
        assert library_path.read_bytes() == original and len(provider.requests) == requests
        # Protected storage, invalid IDs and failed replacement preserve both files.
        for request in (dict(type='export_chat', id=chat, path=str(library_path)),
                        dict(type='export_chat', id=chat, path=str(directory / 'data' / '..' / 'data' / 'library.json')),
                        dict(type='export_chat', id=project, path=str(destination)),
                        dict(type='delete_chat', id='missing'), dict(type='delete_chat', id=project)):
            assert any(e['type'] == 'error' for e in core.turn(request)), request
            assert library_path.read_bytes() == original
            assert json.loads(destination.read_text(encoding='utf-8')) == expected
        locked = k.CreateFileW(str(destination), 0x80000000, 0, None, 3, 0, None)
        assert locked not in (None, -1, 0xffffffffffffffff)
        try:
            assert any(e['type'] == 'error' for e in core.turn(dict(type='export_chat', id=chat, path=str(destination))))
        finally:
            k.CloseHandle(locked)
        assert json.loads(destination.read_text(encoding='utf-8')) == expected
        assert not list(directory.glob('xph*.tmp')), 'Failed export leaked a temporary file'
        sibling = packet(clean(core.turn(dict(type='new_chat', project=project))), 'session')['id']
        clean(core.turn(dict(type='open', id=chat)))
        clean(core.turn(dict(type='delete_chat', id=sibling)))
        assert stored(directory)['active'] == chat
        assert all(s['id'] != sibling for s in stored(directory)['sessions'])
        # Even if its working folder disappeared while closed, deletion must
        # select a valid remaining chat and keep the library's active ID valid.
        core.close()
        moved = directory / 'relocated-project'
        workspace.rename(moved)
        marker = moved / 'keep.txt'
        core = Core(directory)
        clean(core.turn(dict(type='delete_chat', id=chat)))
        library = stored(directory)
        assert library['active'] == root_id
        assert next(p for p in library['projects'] if p['id'] == project)['memory'] == 'KEEP_SHARED_NOTE'
        assert marker.read_text() == 'project file stays'
        # Deleting the sole remaining session creates one empty usable chat.
        clean(core.turn(dict(type='delete_chat', id=root_id)))
        library = stored(directory)
        assert len(library['sessions']) == 1 and library['active'] != root_id
        assert library['sessions'][0]['messages'] == [] and library['sessions'][0]['transcript'] == ''
        assert len(provider.requests) == requests
        # A failed atomic deletion cannot remove the previous on-disk history.
        original = library_path.read_bytes()
        (directory / 'data' / 'library.tmp').mkdir()
        assert any(e['type'] == 'error' for e in core.turn(dict(type='delete_chat', id=library['active'])))
        assert library_path.read_bytes() == original
    finally:
        core.close()
    (directory / 'data' / 'library.tmp').rmdir()
    core = Core(directory)
    try:
        clean(core.turn(dict(type='library')))
        provider.queue(plan('still usable'))
        clean(core.turn(dict(type='chat', text='after deletion restart')))
    finally:
        core.close()


def check_errors(source, directory, provider):
    stage(source, directory, provider)
    core = Core(directory)
    try:
        for stream in (False, True):
            for status, hint in ((401, 'ApiKey'), (403, 'permissions'), (404, 'Chat Completions'),
                                 (429, 'quota'), (500, 'Provider unavailable'), (302, 'final'), (400, 'Model')):
                count = len(provider.requests)
                provider.queue(plan(status=status))
                events = core.turn(dict(type='chat', text='error fixture', stream=stream))
                errors = [e['text'] for e in events if e['type'] == 'error']
                assert len(errors) == 1 and 'HTTP %d' % status in errors[0] and hint in errors[0], errors
                assert len(provider.requests) == count + 1, 'Unexpected automatic retry'
                library = stored(directory)
                active = next(s for s in library['sessions'] if s['id'] == library['active'])
                assert not active['pending']
                tool_pairs(active['messages'])
        provider.queue(plan('continues after error'))
        clean(core.turn(dict(type='chat', text='Continue', stream=True)))
    finally:
        core.close()
    # A bound, non-listening loopback socket refuses connections without DNS/network.
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        (directory / 'vc6ai.ini').write_text('[LLM]\nApiUrl=http://127.0.0.1:%d/chat/completions\nModel=offline\n' % server.getsockname()[1], encoding='ascii')
        core = Core(directory)
        try:
            for stream in (False, True):
                events = core.turn(dict(type='chat', text='connection refused', stream=stream))
                assert any(e['type'] == 'error' and 'Connection failed' in e.get('text', '') for e in events), events
        finally:
            core.close()


def main():
    source = Path(sys.argv[1]).resolve()
    provider = Provider()
    try:
        with tempfile.TemporaryDirectory(prefix='xp-basics-') as temporary:
            root = Path(temporary)
            check_sessions(source, root / 'sessions', provider)
            check_errors(source, root / 'errors', provider)
    finally:
        provider.close()
    print('desktop_basics_smoke: full Unicode export, atomic overwrite/failure, protected storage, active/inactive/last deletion, restart, HTTP hints and refused connections passed')


if __name__ == '__main__':
    main()
