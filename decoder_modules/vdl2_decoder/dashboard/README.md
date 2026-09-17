# SDR++ aviation dashboard

Independent local Flask dashboard adapted from acars-pi-dashboard. It reads the
VDL2 module's JSONL output without accessing DSP objects, starting a decoder,
changing callbacks, or adding work to the receiver thread.

## Run

Requires Python 3.10+ with SQLite. From this directory:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python server.py
```

Open http://127.0.0.1:5050. Enable **File (JSONL)** output in SDR++'s VDL2 module
using `/tmp/aviation_messages.jsonl`. The dashboard can start before SDR++ and
waits for that file. Stop the dashboard with Ctrl+C; the radio is unaffected.
On Windows use `.venv\Scripts\python.exe` and pass the JSONL path explicitly.

Options: `--source PATH`, `--database PATH`, `--port PORT`. Default database:
`~/.sdrpp/aviation-dashboard.sqlite3`. This is separate from SDR++'s existing
SQLite output. The server binds only to 127.0.0.1; there is no public deployment,
autostart service, or packaged-app launcher in this change.

## Message contract

- Schema v2 `freq` is Hz; displayed as MHz. `timestamp` is Unix seconds; displayed
  in UTC. Filters use exact structured values; UTC day is a half-open interval.
- Protocol and transport are separate. CPDLC/ACARS and CPDLC/ATN stay distinct.
- `decoded`, `decode_path`, addresses, frame type, SNR, FEC, PPM, and unknown future
  fields are retained in the full JSON. Expand a message to inspect nested trees.
- No aircraft identity or protocol data is extracted from display text. Old
  records lacking structured classification display as **Legacy record**, with
  unavailable fields left blank. Their original fields remain in complete JSON;
  legacy frequency units are not assumed to be Hz.
- Browser rendering uses text nodes, including for received markup and tree keys.
- The latest 200 messages refresh every three seconds. Pause preserves the view;
  ingestion continues. Older messages pauses live mode and pages backward. Resume
  returns to the newest window. Header counts cover the entire stored archive.

## Clear history

Use **Delete all messages** beside Pause live and confirm the dialog. This deletes
all dashboard records, including records hidden by filters, and advances the reader
to the current end of the JSONL file in the same transaction. A partial line already
in progress is skipped through its newline. New messages continue to be stored.
The source JSONL file is preserved; this does not delete SDR++ logs or its database.
The operation cannot be undone in the dashboard. Replacing/replaying the source
file later can import those records again. Other open dashboard tabs refresh when
live polling resumes. The backend requires an instance token and explicit JSON
confirmation; GET requests cannot delete data.

## Ingestion behavior and limits

Whole records and the byte cursor commit in one SQLite WAL transaction. Restarting
with the same database resumes the same file. Unterminated lines wait for a newline;
invalid JSON/non-object records and lines larger than 4 MiB are counted and skipped.
Record identity is file position, so identical over-air messages are preserved.

Inode replacement, observed truncation, and a changed 128-byte trailing fingerprint
restart reading at zero. Rotate between complete records; do not keep writing to
a renamed file. A truncate/regrow that reproduces identical bytes around the cursor
cannot be distinguished from append. Moving/replaying a file intentionally may
reimport records. Use one reader process per database/source. There is no automatic
archive deletion; disk usage grows with retained traffic. Back up the database
with SQLite's backup API or with the dashboard stopped.

`GET /api/messages` returns `{messages: [{id, message}], next_before}`. Parameters:
`protocol`, `transport`, `direction`, `tail`, `flight`, `freq` (Hz), `since`, `until`
(Unix seconds), `before` (record ID), `limit` (1–500). Each `message` is the complete
original JSON object. `GET /api/status` reports reader status, skipped lines,
archive count, protocol/transport counts, and frequencies.

## Tests

```sh
.venv/bin/python -m unittest discover -s tests -v
```

Tests cover all requested protocol/transport classes, complete tree and metadata
preservation, partial lines, restart without duplication, rotation, truncate/regrow,
malformed/oversized records, missing files, pagination/filtering, and transaction
rollback. Protocol-class fixtures here test consumption, not decoding accuracy;
the parent module's C++ suite tests decoding and reassembly.

See [THIRD_PARTY.md](THIRD_PARTY.md) for upstream revision and licensing.

## Launch from the VDL2 module (macOS/Linux)

Expand **Web dashboard** in the VDL2 module. Select the Python executable in
this dashboard's virtual environment and this directory's `server.py`, then
press **Start dashboard**. The default port is 5057; open the displayed URL in
your browser. The module enables JSON File output and passes its current JSONL
path to the server. Restart the dashboard after changing that path.

Create the virtual environment and install `requirements.txt` once using the
setup instructions above. The module does not install Python or packages.
Paths and port are saved in the module configuration. Development builds default
to the source dashboard directory; a relocated installation must select its own
paths (or place the dashboard at `<SDR++ config root>/vdl2-dashboard/`). Python
and dashboard assets are not bundled automatically.

**Stop dashboard** stops only the process launched by this module. Radio
Start/Stop leaves it available for browsing history; unloading the module or
exiting SDR++ stops its child server. Closing the parent process also closes a
private lifetime pipe so the server exits. Shutdown has a bounded fallback for
an unresponsive child. Manually launched servers are never terminated: a port
conflict is reported in `<SDR++ config root>/vdl2-dashboard.log`. The UI reports
process state; the log confirms HTTP readiness and includes Python errors.
Windows users can continue launching `server.py` manually.
