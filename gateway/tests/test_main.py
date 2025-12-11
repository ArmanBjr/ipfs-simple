from fastapi.testclient import TestClient

from gateway.main import app

client = TestClient(app)


def test_root_page_renders_index_template():
    response = client.get("/")
    assert response.status_code == 200
    assert "FUM Content Storage Gateway" in response.text


def test_docs_page_renders_docs_template():
    response = client.get("/docs")
    assert response.status_code == 200
    assert "Gateway Documentation" in response.text


def test_dashboard_requires_auth():
    response = client.get("/dashboard")
    assert response.status_code == 401
