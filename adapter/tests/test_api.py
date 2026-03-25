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
