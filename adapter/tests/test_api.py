from fastapi.testclient import TestClient

from adapter import service


client = TestClient(service.app)


def test_health():
    r = client.get('/health')
    assert r.status_code == 200
    body = r.json()
    assert body['ok'] is True


def test_submit_and_get_job(monkeypatch, tmp_path):
    input_file = tmp_path / 'job.pdl'
    input_file.write_text('x')

    captured = {}

    def fake_start_job(job_id, payload):
        captured['job_id'] = job_id
        with service._lock:
            job = service._jobs[job_id]
            job['status'] = 'completed'
            job['updated_at'] = service._utc_now()
            job['exit_code'] = 0
            job['events'].append({'event': 'rip.completed'})
        return ['memjet-rip', payload.input_path]

    monkeypatch.setattr(service, 'start_job', fake_start_job)

    post = client.post('/jobs', json={'input_path': str(input_file)})
    assert post.status_code == 202
    jid = post.json()['id']

    get = client.get(f'/jobs/{jid}')
    assert get.status_code == 200
    body = get.json()
    assert body['status'] == 'completed'
    assert body['exit_code'] == 0


def test_submit_invalid_path():
    r = client.post('/jobs', json={'input_path': '/no/such/file'})
    assert r.status_code == 422


def test_injects_hard_defaults_without_env(monkeypatch, tmp_path):
    input_file = tmp_path / 'job.pdf'
    input_file.write_text('x')

    monkeypatch.delenv('RIP_DEFAULT_PES_IP', raising=False)
    monkeypatch.delenv('RIP_PES_IP', raising=False)
    monkeypatch.delenv('PES_IP', raising=False)
    monkeypatch.delenv('RIP_DEFAULT_PES_PORT', raising=False)
    monkeypatch.delenv('RIP_PES_PORT', raising=False)
    monkeypatch.delenv('PES_PORT', raising=False)

    def fake_run_job(job_id, command, env_overrides):
        pass

    class FakeThread:
        def __init__(self, target=None, args=(), daemon=None):
            self._target = target
            self._args = args

        def start(self):
            self._target(*self._args)

    monkeypatch.setattr(service, '_default_command', lambda: ['memjet-rip'])
    monkeypatch.setattr(service.threading, 'Thread', FakeThread)
    monkeypatch.setattr(service, '_run_job', fake_run_job)

    command = service.start_job('job-1', service.JobRequest(input_path=str(input_file)))

    assert '--pes-ip' in command
    assert '192.168.111.2' in command
    assert '--pes-port' in command
    assert '13001' in command


def test_explicit_args_override_defaults(monkeypatch, tmp_path):
    input_file = tmp_path / 'job.pdf'
    input_file.write_text('x')

    monkeypatch.setenv('RIP_DEFAULT_PES_IP', '192.168.111.2')
    monkeypatch.setenv('RIP_DEFAULT_PES_PORT', '13001')

    captured = {}

    def fake_run_job(job_id, command, env_overrides):
        captured['command'] = command

    class FakeThread:
        def __init__(self, target=None, args=(), daemon=None):
            self._target = target
            self._args = args

        def start(self):
            self._target(*self._args)

    monkeypatch.setattr(service, '_default_command', lambda: ['memjet-rip'])
    monkeypatch.setattr(service.threading, 'Thread', FakeThread)
    monkeypatch.setattr(service, '_run_job', fake_run_job)

    command = service.start_job(
        'job-2',
        service.JobRequest(
            input_path=str(input_file),
            args=['--pes-ip', '10.0.0.1', '--pes-port', '9040'],
        ),
    )

    assert command.count('--pes-ip') == 1
    assert '10.0.0.1' in command
    assert command.count('--pes-port') == 1
    assert '9040' in command
