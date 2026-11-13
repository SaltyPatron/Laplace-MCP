"""What the Laplace-MCP database checks share: the Engine's check helper, the deployed lpm, and lpm's two ways in.

The guards live once, in the Engine's tests/deployed/check.py (LAPLACE_ENGINE_SOURCE: the Engine checkout of the
deployed revision); this module loads it and adds only what lpm needs. lpm is the deployed one (LAPLACE_LPM, the
release's bin/lpm). Its launcher pins LAPLACE_ENGINE, LAPLACE_RECIPES, the tier-0, highway and grammar artifacts and
the firmware to the release; LAPLACE_CONNINFO and everything else come from the environment given here.

lpm is reached two ways: `lpm mcp`, one JSON-RPC exchange on standard input and output (no listener), and `lpm serve`
on a free loopback port with a bearer token made for the check, never the deployed service's port or credential. This
file's name begins with an underscore: run.sh runs every other tests/deployed/*.py as a check.
"""
import importlib.util
import json
import os
import secrets
import socket
import subprocess
import time
import urllib.request
from pathlib import Path

SERVICE_PORT = 5188                       # the deployed service's listener: a check never binds or calls it


def engine_check():
    source = os.environ.get('LAPLACE_ENGINE_SOURCE', '').strip()
    path = Path(source) / 'tests/deployed/check.py' if source else None
    if not path or not path.is_file():
        raise SystemExit('LAPLACE_ENGINE_SOURCE must name the deployed Engine checkout (tests/deployed/check.py)')
    spec = importlib.util.spec_from_file_location('laplace_engine_check', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = engine_check()


def source(fixture, part, **arguments):
    """A source of the check (check.Fixture.source) whose witness is also forgotten by its name. `laplace forget
    SOURCE` finds a source's witness through LAPLACE_RECIPES, which the release launcher of the deployed program pins
    to the release's recipes, where a check source is not; the witness's own name needs no recipes."""
    name, root = fixture.source(part, **arguments)
    fixture.witness(part)
    return name, root


def ingest(fixture, name, timeout=900):
    """Ingest one check source with the check's recipes named by -r, which the launcher's LAPLACE_RECIPES does not
    override."""
    return fixture.run('ingest', '-r', fixture.recipes, '-j', '2', name, timeout=timeout)


def lpm():
    value = os.environ.get('LAPLACE_LPM', '')
    if not value or not Path(value).is_file():
        raise SystemExit('LAPLACE_LPM must name the deployed lpm (the release bin/lpm)')
    return value


def environment(fixture, **more):
    """lpm's environment for a check: the deployed database, the check's scratch, and nothing of the service's
    listener, credential or code index."""
    env = dict(os.environ)
    for name in ('LAPLACE_MCP_TOKEN', 'LAPLACE_MCP_TOKEN_FILE', 'LAPLACE_MCP_HOST', 'LAPLACE_MCP_PORT',
                 'LAPLACE_CODE_ROOTS', 'LAPLACE_CODE_STORE', 'LAPLACE_CODE_RECORDS', 'LAPLACE_INGEST_LOGS',
                 'LAPLACE_INGEST_ONE', 'PGAPPNAME'):
        env.pop(name, None)
    code = fixture.scratch / 'code'
    code.mkdir(exist_ok=True)
    env.update(LAPLACE_CONNINFO=check.connection(), LAPLACE_WORK=str(fixture.work), LAPLACE_DATA=str(fixture.data),
               LAPLACE_RECIPES=str(fixture.recipes), LAPLACE_CODE_STORE=str(fixture.scratch / 'code-store'),
               LAPLACE_CODE_ROOTS=str(code), LAPLACE_CODE_SEMANTIC='')
    env.update({k: str(v) for k, v in more.items()})
    return env


def mcp(fixture, tool, arguments=None, **more):
    """One MCP exchange through `lpm mcp` (standard input and output): tools/list when tool is None, else the tool's
    JSON answer. A tool error raises."""
    messages = [{'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {}},
                {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call' if tool else 'tools/list',
                 'params': {'name': tool, 'arguments': arguments or {}} if tool else {}}]
    result = subprocess.run([lpm(), 'mcp'], input='\n'.join(json.dumps(m) for m in messages) + '\n',
                            env=environment(fixture, **more), capture_output=True, text=True, timeout=300)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        raise AssertionError(f'lpm mcp {tool}: no answer (exit {result.returncode}): {result.stderr}')
    answer = json.loads(lines[-1])
    if 'result' not in answer:
        raise AssertionError(f'lpm mcp {tool}: {answer}')
    answer = answer['result']
    if not tool:
        return answer
    if answer.get('isError'):
        raise AssertionError(f'{tool} {arguments}: {answer["content"][0]["text"]}')
    payload = answer['content'][0]['text']
    try:
        return json.loads(payload)
    except json.JSONDecodeError as error:
        raise AssertionError(f'{tool} returned malformed JSON: {payload!r}') from error


def free_port():
    while True:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        if port != SERVICE_PORT:
            return port


class Server:
    """`lpm serve` on a free loopback port with the check's own token, its sessions named `application` (PGAPPNAME,
    so pg_stat_activity can be read for this server's connections alone). Stopped on the way out."""

    def __init__(self, fixture, application, **more):
        self.token = secrets.token_hex(32)
        self.port = free_port()
        self.application = application
        self.log_path = fixture.scratch / f'serve-{application}.log'
        env = environment(fixture, LAPLACE_MCP_TOKEN=self.token, PGAPPNAME=application, **more)
        self.log = self.log_path.open('w+', encoding='utf-8')
        self.process = subprocess.Popen([lpm(), 'serve', '127.0.0.1', str(self.port)], env=env,
                                        stdout=self.log, stderr=self.log)

    def __enter__(self):
        deadline = time.monotonic() + 30
        while True:
            try:
                if self.request('/health') == {'ok': True}:
                    return self
            except (OSError, TimeoutError):
                pass
            if self.process.poll() is not None or time.monotonic() > deadline:
                self.log.flush()
                raise RuntimeError('lpm serve did not answer: ' + self.log_path.read_text(encoding='utf-8'))
            time.sleep(.1)

    def __exit__(self, *exc):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
        self.log.close()
        return False

    def request(self, path, data=None, timeout=120):
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}' + path,
                                     data=None if data is None else json.dumps(data).encode(),
                                     headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + self.token})
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return json.load(response)
