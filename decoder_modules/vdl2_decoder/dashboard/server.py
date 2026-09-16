#!/usr/bin/env python3
"""Independent SDR++ JSONL dashboard. Never controls the radio or decoder."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import secrets
import threading
import time

from flask import Flask, jsonify, request

MAX_LINE = 4 * 1024 * 1024
FIELDS = ('protocol', 'transport', 'direction', 'tail', 'flight')


@contextmanager
def connect(path):
    db = sqlite3.connect(path, timeout=10)
    db.row_factory = sqlite3.Row
    try:
        with db:
            yield db
    finally:
        db.close()


def initialize(path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with connect(path) as db:
        db.execute('PRAGMA journal_mode=WAL')
        db.executescript('''
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY, timestamp REAL, freq REAL,
                protocol TEXT, transport TEXT, direction TEXT, tail TEXT, flight TEXT,
                raw TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS message_time ON messages(timestamp);
            CREATE INDEX IF NOT EXISTS message_protocol ON messages(protocol, transport);
            CREATE TABLE IF NOT EXISTS dashboard_state (
                id INTEGER PRIMARY KEY CHECK(id=1), generation INTEGER NOT NULL);
            INSERT OR IGNORE INTO dashboard_state VALUES (1,0);
            CREATE TABLE IF NOT EXISTS cursor (
                source TEXT PRIMARY KEY, device INTEGER, inode INTEGER,
                offset INTEGER, anchor TEXT, skipping INTEGER, invalid INTEGER);
        ''')


def number(value):
    if type(value) not in (int, float):
        return None
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except OverflowError:
        return None


def reject_constant(value):
    raise ValueError('Non-JSON number: ' + value)


class Reader:
    """Commit whole lines and file cursor in one transaction; leave partial lines.

    A trailing-byte fingerprint detects truncate/rewrite even when the file grows
    past the previous offset between polls. Replaced files start at byte zero.
    The writer must rotate between records; bytes appended to an already renamed
    file after replacement cannot be recovered by a pathname-only consumer.
    """
    def __init__(self, source, database):
        self.source = str(Path(source).resolve())
        self.database = str(database)
        initialize(database)
        self.ingest_lock = threading.Lock()
        self.lock = threading.Lock()
        self.state = {'source': self.source, 'state': 'waiting', 'error': None}

    def status(self):
        with self.lock:
            return dict(self.state)

    def fingerprint(self, file, offset):
        file.seek(max(0, offset - 128))
        return hashlib.sha256(file.read(min(offset, 128))).hexdigest()

    def poll(self):
        try:
            with self.ingest_lock:
                count = self._poll()
            with self.lock:
                self.state.update(state='watching', error=None, checked_at=time.time())
            return count
        except FileNotFoundError:
            with self.lock:
                self.state.update(state='waiting', error='Waiting for JSONL file')
            return 0
        except (OSError, sqlite3.Error) as exc:
            with self.lock:
                self.state.update(state='error', error=str(exc))
            return 0

    def _poll(self):
        with open(self.source, 'rb') as file, connect(self.database) as db:
            stat = os.fstat(file.fileno())
            db.execute('BEGIN IMMEDIATE')
            old = db.execute('SELECT * FROM cursor WHERE source=?', (self.source,)).fetchone()
            offset, skipping, invalid = 0, False, old['invalid'] if old else 0
            if (old and (old['device'], old['inode']) == (stat.st_dev, stat.st_ino)
                    and stat.st_size >= old['offset']
                    and self.fingerprint(file, old['offset']) == old['anchor']):
                offset, skipping = old['offset'], bool(old['skipping'])
            file.seek(offset)
            count = 0
            # Bound work per transaction and memory even for malformed input.
            for _ in range(500):
                line = file.readline(MAX_LINE + 1)
                if not line:
                    break
                if skipping or len(line) > MAX_LINE:
                    if not skipping:
                        invalid += 1
                    skipping = not line.endswith(b'\n')
                    offset = file.tell()
                    continue
                if not line.endswith(b'\n'):
                    break
                offset = file.tell()
                try:
                    msg = json.loads(line, parse_constant=reject_constant)
                    if not isinstance(msg, dict):
                        raise ValueError('Expected object')
                    values = [msg.get(k) if isinstance(msg.get(k), str) else None for k in FIELDS]
                    db.execute('''INSERT INTO messages
                        (timestamp,freq,protocol,transport,direction,tail,flight,raw)
                        VALUES (?,?,?,?,?,?,?,?)''',
                        (number(msg.get('timestamp')), number(msg.get('freq')) if msg.get('schema_version') == 2 else None, *values,
                         json.dumps(msg, ensure_ascii=True, allow_nan=False)))
                    count += 1
                except (ValueError, UnicodeError, RecursionError):
                    invalid += 1
            anchor = self.fingerprint(file, offset)
            cursor = (self.source, stat.st_dev, stat.st_ino, offset, anchor, skipping, invalid)
            if old is None or tuple(old) != cursor:
                db.execute('INSERT OR REPLACE INTO cursor VALUES (?,?,?,?,?,?,?)', cursor)
        with self.lock:
            self.state.update(offset=offset, invalid_lines=invalid)
            if count:
                self.state['last_received_at'] = time.time()
        return count

    def delete_all(self):
        # Serialize with ingestion; history and the skip cursor commit together.
        with self.ingest_lock, connect(self.database) as db:
            db.execute('BEGIN IMMEDIATE')
            try:
                with open(self.source, 'rb') as file:
                    stat = os.fstat(file.fileno())
                    offset = stat.st_size
                    file.seek(max(0, offset - 1))
                    skipping = offset > 0 and file.read(1) != b'\n'
                    anchor = self.fingerprint(file, offset)
                    db.execute('INSERT OR REPLACE INTO cursor VALUES (?,?,?,?,?,?,?)',
                               (self.source, stat.st_dev, stat.st_ino, offset, anchor, skipping, 0))
            except FileNotFoundError:
                offset = 0
                db.execute('DELETE FROM cursor WHERE source=?', (self.source,))
            deleted = db.execute('SELECT COUNT(*) FROM messages').fetchone()[0]
            db.execute('DELETE FROM messages')
            db.execute('UPDATE dashboard_state SET generation=generation+1 WHERE id=1')
            # Commit before publishing status. On error both changes roll back.
            db.commit()
            with self.lock:
                self.state.update(offset=offset, invalid_lines=0)
                self.state.pop('last_received_at', None)
            return deleted

    def run(self, stop):
        while not stop.is_set():
            count = self.poll()
            if count < 500:
                stop.wait(0.5)


def create_app(database, reader):
    app = Flask(__name__, static_folder='static', static_url_path='/static')
    delete_token = secrets.token_urlsafe(32)

    @app.after_request
    def headers(response):
        response.headers['Cache-Control'] = 'no-store'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Content-Security-Policy'] = "default-src 'self'; object-src 'none'; frame-ancestors 'none'"
        return response

    @app.get('/')
    def index():
        return app.send_static_file('index.html')

    @app.post('/api/messages/delete-all')
    def delete_all():
        # Browser callers must read this instance's token through same-origin JSON.
        # No CORS is enabled; a cross-site form cannot supply this custom header.
        if not secrets.compare_digest(request.headers.get('X-Dashboard-Token', ''), delete_token):
            return jsonify(error='Reload the dashboard before deleting messages'), 403
        if not request.is_json or request.get_json(silent=True) != {'confirm': True}:
            return jsonify(error='Explicit confirmation required'), 400
        try:
            return jsonify(deleted=reader.delete_all())
        except (OSError, sqlite3.Error):
            app.logger.exception('Unable to clear dashboard history')
            return jsonify(error='Could not clear history; please try again'), 500

    @app.get('/api/messages')
    def messages():
        try:
            before = max(0, int(request.args.get('before', 0)))
            limit = min(500, max(1, int(request.args.get('limit', 200))))
        except ValueError:
            return jsonify(error='Invalid cursor or limit'), 400
        clauses, params = [], []
        for field in FIELDS:
            value = request.args.get(field)
            if value:
                clauses.append(field + ' = ?')
                params.append(value)
        for field in ('freq', 'since', 'until'):
            value = request.args.get(field)
            if value:
                try:
                    value = float(value)
                    if not math.isfinite(value):
                        raise ValueError()
                except ValueError:
                    return jsonify(error='Invalid ' + field), 400
                clauses.append({'freq': 'freq = ?', 'since': 'timestamp >= ?',
                                'until': 'timestamp < ?'}[field])
                params.append(value)
        if before:
            clauses.append('id < ?')
            params.append(before)
        where = ' WHERE ' + ' AND '.join(clauses) if clauses else ''
        with connect(database) as db:
            db.execute('BEGIN')
            generation = db.execute('SELECT generation FROM dashboard_state WHERE id=1').fetchone()[0]
            rows = db.execute('SELECT id,raw FROM messages' + where + ' ORDER BY id DESC LIMIT ?',
                              (*params, limit + 1)).fetchall()
        page = rows[:limit]
        return jsonify(generation=generation, messages=[{'id': r['id'], 'message': json.loads(r['raw'])} for r in page],
                       next_before=page[-1]['id'] if len(rows) > limit else None)

    @app.get('/api/status')
    def status():
        with connect(database) as db:
            count = db.execute('SELECT COUNT(*) FROM messages').fetchone()[0]
            paths = [dict(r) for r in db.execute('SELECT protocol,transport,COUNT(*) AS count FROM messages GROUP BY protocol,transport')]
            frequencies = [r[0] for r in db.execute('SELECT DISTINCT freq FROM messages WHERE freq IS NOT NULL ORDER BY freq')]
        return jsonify(delete_token=delete_token, reader=reader.status(), total=count, paths=paths, frequencies=frequencies)

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='/tmp/aviation_messages.jsonl')
    parser.add_argument('--database', default=str(Path.home() / '.sdrpp/aviation-dashboard.sqlite3'))
    parser.add_argument('--port', type=int, default=5050)
    args = parser.parse_args()
    reader = Reader(args.source, args.database)
    stop = threading.Event()
    thread = threading.Thread(target=reader.run, args=(stop,), name='aviation-jsonl')
    thread.start()
    try:
        create_app(args.database, reader).run(host='127.0.0.1', port=args.port, threaded=True, use_reloader=False)
    finally:
        stop.set()
        thread.join()


if __name__ == '__main__':
    main()
