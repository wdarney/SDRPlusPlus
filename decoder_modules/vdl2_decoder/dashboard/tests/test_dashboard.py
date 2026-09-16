import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import Reader, create_app, MAX_LINE, connect


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / 'messages.jsonl'
        self.database = Path(self.temp.name) / 'dashboard.sqlite3'
        self.reader = Reader(self.source, self.database)
        self.client = create_app(self.database, self.reader).test_client()

    def write(self, *messages):
        with self.source.open('ab') as file:
            for msg in messages:
                file.write(json.dumps(msg).encode() + b'\n')

    def records(self, query=''):
        return self.client.get('/api/messages' + query).get_json()['messages']

    def test_protocols_and_complete_tree(self):
        msgs = []
        for protocol, transport in [('VDL2', None), ('ACARS', 'ACARS'), ('CPDLC', 'ACARS'),
                                    ('CPDLC', 'ATN'), ('ADS-C', 'ATN'), ('CM', 'ATN')]:
            msg = dict(schema_version=2, timestamp=1720000000.25, freq=136975000, protocol=protocol,
                       transport=transport, tail='N795UA', flight='UA0884', direction='AIR2GND',
                       src={'type':'AIR','address':'AACC5B'}, dst={'type':'GND','address':'102C3A'},
                       snr=28, fec=0, ppm=-0.7, frame_type='I',
                       decoded={'test_fixture': {'nested': [{'value': 0, 'present': False}]}},
                       text='<script>alert("untrusted RF text")</script>')
            msgs.append(msg)
        self.write(*msgs)
        self.assertEqual(self.reader.poll(), 6)
        self.assertEqual([r['message'] for r in reversed(self.records())], msgs)
        self.assertEqual(len(self.records('?protocol=CPDLC&transport=ATN')), 1)
        self.assertEqual(len(self.records('?protocol=CPDLC&transport=ACARS')), 1)
        self.assertEqual(len(self.records('?tail=N795UA&flight=UA0884&direction=AIR2GND&freq=136975000')), 6)
        self.assertEqual(len(self.records('?since=1720000000&until=1720000001')), 6)
        self.assertEqual(len(self.records('?since=1720000001')), 0)

    def test_partial_and_restart(self):
        self.source.write_bytes(b'{"protocol":"CM"')
        self.assertEqual(self.reader.poll(), 0)
        with self.source.open('ab') as f: f.write(b'}\n')
        self.assertEqual(self.reader.poll(), 1)
        restarted = Reader(self.source, self.database)
        self.assertEqual(restarted.poll(), 0)
        self.write({'protocol':'CPDLC'})
        self.assertEqual(restarted.poll(), 1)
        self.assertEqual(len(self.records()), 2)

    def test_rotation_and_truncate_regrow(self):
        self.write({'text':'first'})
        self.reader.poll()
        self.source.rename(self.source.with_suffix('.old'))
        self.write({'text':'replacement'})
        self.assertEqual(self.reader.poll(), 1)
        self.source.write_text(json.dumps({'text':'rewritten with a longer line than before'})+'\n')
        self.assertEqual(self.reader.poll(), 1)
        self.assertEqual(len(self.records()), 3)
        self.source.write_bytes(b'')
        self.reader.poll()
        self.write({'text':'after empty file'})
        self.assertEqual(self.reader.poll(), 1)

    def test_missing_invalid_and_oversize(self):
        self.reader.poll()
        self.assertEqual(self.reader.status()['state'], 'waiting')
        self.source.write_bytes(b'invalid\n[]\n{"a":NaN}\n\xff\n' + b'x'*(MAX_LINE+20))
        self.reader.poll()
        with self.source.open('ab') as f: f.write(b'end\n{"protocol":"CM"}\n')
        self.assertEqual(self.reader.poll(), 1)
        self.assertEqual(self.reader.status()['invalid_lines'], 5)
        self.assertEqual(len(self.records()), 1)

    def test_pagination_and_validation(self):
        self.write(*[{'text':str(i)} for i in range(6)])
        self.reader.poll()
        first=self.client.get('/api/messages?limit=2').get_json()
        second=self.client.get('/api/messages?limit=2&before='+str(first['next_before'])).get_json()
        self.assertEqual([r['message']['text'] for r in first['messages']+second['messages']], ['5','4','3','2'])
        for q in ('limit=bad', 'before=nan', 'freq=nan', 'since=bad'):
            self.assertEqual(self.client.get('/api/messages?'+q).status_code, 400)
        self.assertEqual(self.records("?tail='OR%201=1--"), [])
        self.assertEqual(self.client.get('/api/status').get_json()['total'], 6)
        page=self.client.get('/')
        self.assertEqual(page.status_code, 200)
        self.assertIn("default-src 'self'", page.headers['Content-Security-Policy'])
        page.close()
        with self.client.get('/static/app.js') as script:
            self.assertEqual(script.status_code, 200)

    def test_background_reader_append_and_shutdown(self):
        stop = threading.Event()
        thread = threading.Thread(target=self.reader.run, args=(stop,))
        thread.start()
        try:
            for i in range(30):
                self.write({'protocol':'CM', 'transport':'ATN', 'decoded':{'sequence':i}})
                self.assertEqual(self.client.get('/api/messages').status_code, 200)
            deadline = time.monotonic() + 5
            while len(self.records()) < 30 and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertEqual(len(self.records()), 30)
        finally:
            stop.set()
            thread.join(5)
        self.assertFalse(thread.is_alive())

    def test_legacy_frequency_and_large_numbers(self):
        legacy = {'timestamp':'2026-09-15T22:57:19Z', 'freq':136.975, 'type':'ACARS', 'text':'old'}
        huge = {'timestamp':10**400, 'freq':10**400, 'schema_version':2}
        self.write(legacy, huge)
        self.assertEqual(self.reader.poll(), 2)
        self.assertEqual(self.reader.poll(), 0)
        self.assertEqual(self.records()[-1]['message'], legacy)
        self.assertEqual(self.client.get('/api/status').get_json()['frequencies'], [])

    def test_transaction_rollback_does_not_advance_cursor(self):
        self.write({'protocol':'CM'})
        with connect(self.database) as db:
            db.execute("CREATE TRIGGER fail BEFORE INSERT ON messages BEGIN SELECT RAISE(ABORT, 'injected'); END")
        self.assertEqual(self.reader.poll(), 0)
        self.assertEqual(self.reader.status()['state'], 'error')
        with connect(self.database) as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM cursor').fetchone()[0], 0)
            db.execute('DROP TRIGGER fail')
        self.assertEqual(self.reader.poll(), 1)
        self.assertEqual(len(self.records()), 1)


if __name__ == '__main__':
    unittest.main()
