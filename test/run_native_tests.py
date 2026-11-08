#!/usr/bin/env python3
"""Run input native tests against an owned loopback TLS fixture."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parent.parent


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'input-owned curl'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed: {result.stdout}')


def main():
    if len(sys.argv) < 2:
        raise SystemExit('usage: run_native_tests.py EXECUTABLE [ARG ...]')
    temporary = ROOT / 'temp'
    temporary.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='input-curl-tls-', dir=temporary) as directory:
        base = Path(directory)
        ca, ca_key = base / 'ca.pem', base / 'ca.key'
        cert, key, csr, ext = base / 'server.pem', base / 'server.key', base / 'server.csr', base / 'extensions.cnf'
        wrong_ca = base / 'wrong-ca.pem'
        ext.write_text('basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n')
        run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
             '-subj', '/CN=Input Test CA', '-addext', 'basicConstraints=critical,CA:TRUE',
             '-keyout', str(ca_key), '-out', str(ca)])
        run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
             '-keyout', str(key), '-out', str(csr)])
        run(['openssl', 'x509', '-req', '-in', str(csr), '-CA', str(ca), '-CAkey', str(ca_key),
             '-CAcreateserial', '-days', '1', '-sha256', '-extfile', str(ext), '-out', str(cert)])
        run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
             '-subj', '/CN=Wrong Input CA', '-addext', 'basicConstraints=critical,CA:TRUE',
             '-keyout', str(base / 'wrong-ca.key'), '-out', str(wrong_ca)])
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), FixtureHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_address[1]
        env = dict(os.environ, INPUT_CURL_FIXTURE_URL=f'https://localhost:{port}/',
                   INPUT_CURL_FIXTURE_IP_URL=f'https://127.0.0.1:{port}/',
                   INPUT_CURL_FIXTURE_CA=str(ca), INPUT_CURL_FIXTURE_WRONG_CA=str(wrong_ca),
                   INPUT_CURL_FIXTURE_RESOLVE=f'localhost:{port}:127.0.0.1',
                   SSL_CERT_FILE=str(ca))
        env.pop('SSLKEYLOGFILE', None)
        try:
            args = [arg.replace('{fixture_url}', env['INPUT_CURL_FIXTURE_URL'])
                    .replace('{fixture_ip_url}', env['INPUT_CURL_FIXTURE_IP_URL']) for arg in sys.argv[1:]]
            return subprocess.run(args, cwd=env.get('INPUT_CURL_FIXTURE_CWD', str(ROOT)), env=env, timeout=60).returncode
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    raise SystemExit(main())
