#!/usr/bin/env python3
"""HTTP contract and bounded-shutdown regression checks; no SDR required."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
from urllib.parse import urlsplit

binary, assets = sys.argv[1:3]
process = subprocess.Popen([binary, assets], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
try:
    url = urlsplit(process.stdout.readline().strip())
    assert url.port, "server did not start"
    def request(path, method="GET", headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", url.port, timeout=5)
        conn.request(method, path, headers=headers or {})
        response = conn.getresponse()
        body = response.read()
        result = response.status, body, dict(response.getheaders())
        conn.close()
        return result
    status, body, headers = request("/")
    assert status == 200 and b"tar1090" in body and int(headers["Content-Length"]) == len(body)
    status, body, headers = request("/libs/ol-custom-10.9.0.js")
    assert status == 200 and len(body) > 100000 and int(headers["Content-Length"]) == len(body)
    status, body, _ = request("/basic-world.geojson")
    assert status == 200 and len(json.loads(body)["features"]) == 1493
    assert request("/sdrpp_basic_map.js")[0] == 200
    assert request("/", "HEAD")[1] == b""
    assert request("/", "POST")[0] == 405
    assert request("/", headers={"Host": "untrusted.example"})[0] == 403
    for path in ["/../THIRD_PARTY.md", "/%2e%2e/THIRD_PARTY.md", "/..\\THIRD_PARTY.md"]:
        assert request(path)[0] == 400, path
    assert request("/absent.js")[0] == 404
    assert request("/upintheair.json")[0] == 404  # {} breaks tar1090's optional outline renderer
    assert json.loads(request("/db2/ranges.js")[1]) == {"military": []}
    aircraft = json.loads(request("/data/aircraft.json")[1])
    assert aircraft["aircraft"] == [] and abs(aircraft["now"] - time.time()) < 2
    receiver = json.loads(request("/data/receiver.json")[1])
    assert receiver["zstd"] is False and receiver["history"] == 0
    assert request("/data/history_99999999999999999999.json")[0] == 404
    # Header fragmentation is normal on a stream socket.
    client = socket.create_connection(("127.0.0.1", url.port), timeout=3)
    client.sendall(b"GET /data/aircraft.json HTTP/1.1\r\nHo")
    client.sendall(f"st: 127.0.0.1:{url.port}\r\n\r\n".encode())
    assert client.recv(1024).startswith(b"HTTP/1.1 200")
    client.close()
    # An unfinished request must not hang module shutdown.
    slow = socket.create_connection(("127.0.0.1", url.port), timeout=3)
    slow.sendall(b"GET / HTTP/1.1\r\n")
    start = time.monotonic()
    process.stdin.write("\n"); process.stdin.flush()
    process.wait(timeout=2)
    assert time.monotonic() - start < 1.5
    slow.close()
    assert process.returncode == 0
    print("PASS: assets, JSON schema, HEAD, read-only methods, host/path checks, fragmented headers, bounded shutdown")
finally:
    if process.poll() is None:
        process.terminate(); process.wait(timeout=3)
