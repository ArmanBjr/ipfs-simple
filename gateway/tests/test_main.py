from fastapi.testclient import TestClient
from gateway.main import app

client = TestClient(app)

def test_root_page_renders_index_template():
    response = client.get("/")
    assert response.status_code == 200
    assert "IPFS Project" in response.text

def test_docs_page_renders_docs_template():
    response = client.get("/docs")
    assert response.status_code == 200
    assert "IPFS Documentation" in response.text

def test_try_page_returns_plain_text():
    response = client.get("/try")
    assert response.status_code == 200
    assert response.text == '"try page"'
