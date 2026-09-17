"""Exercise the actual HTTP process and module lifetime pipe using isolated files."""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

SERVER = pathlib.Path(__file__).resolve().parents[1] / 'server.py'


class ManagedServerTests(unittest.TestCase):
    def test_parent_pipe_and_port_collision(self):
        with tempfile.TemporaryDirectory() as folder:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            command = [sys.executable, str(SERVER), '--managed', '--port', str(port),
                       '--source', folder + '/messages.jsonl', '--database', folder + '/history.sqlite3']
            with open(folder + '/log', 'wb') as log:
                for _ in range(3):
                    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=log)
                    try:
                        for attempt in range(100):
                            self.assertIsNone(child.poll(), 'server exited during startup')
                            try:
                                with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/status', timeout=1) as reply:
                                    self.assertEqual(reply.status, 200)
                                break
                            except OSError:
                                time.sleep(.05)
                        else:
                            self.fail('server did not become ready')
                        collision = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=log)
                        try:
                            self.assertNotEqual(collision.wait(timeout=10), 0)
                            self.assertIsNone(child.poll())
                        finally:
                            collision.stdin.close()
                            if collision.poll() is None:
                                collision.kill()
                                collision.wait()
                        child.stdin.close()  # module Stop / destruction / parent exit
                        self.assertEqual(child.wait(timeout=10), 0)
                    finally:
                        if not child.stdin.closed:
                            child.stdin.close()
                        if child.poll() is None:
                            child.kill()
                            child.wait()
