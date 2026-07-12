#!/usr/bin/env python3
"""
Convert RadioReference CSV exports into SDR++ Frequency Manager bookmark groups.

Names are taken from the Alpha Tag (short) with common ARTCC/facility prefixes
stripped. The full RR Description is stored in an optional "description" field
that the channel_bank module shows on hover.

Usage:
    # Single CSV -> single bookmark group file (matches SDR++ "Export Group")
    python3 rr_to_sdrpp.py input.csv -o output.json

    # Multiple CSVs -> frequency_manager_config.json with one list per CSV
    python3 rr_to_sdrpp.py file1.csv file2.csv -o frequency_manager_config.json

    # Override list name
    python3 rr_to_sdrpp.py input.csv -o output.json --name "ZNY ARTCC"

    # Keep long Alpha Tag prefixes intact
    python3 rr_to_sdrpp.py input.csv -o output.json --no-shorten

Mode mapping (SDR++):  0=NFM, 1=WFM, 2=AM, 3=USB, 4=LSB, 5=CW, 6=RAW
"""

import argparse
import csv
import json
import os
import re
import sys

MODE_MAP = {
    "AM":  2, "FM":  0, "NFM": 0, "FMN": 0, "WFM": 1,
    "USB": 3, "LSB": 4, "CW":  5,
}

DEFAULT_BW = {
    0: 12500.0, 1: 150000.0, 2: 8300.0, 3: 2800.0, 4: 2800.0, 5: 500.0, 6: 12500.0,
}

# Prefixes like "ZNY91 ", "ZBW53 " — 3-4 letter ARTCC id + optional number + space
PREFIX_RE = re.compile(r"^[A-Z]{3,4}\d*\s+")


def short_name(alpha, desc):
    """Return a concise, readable bookmark name."""
    raw = (alpha or desc or "").strip()
    cleaned = PREFIX_RE.sub("", raw).strip()
    return cleaned or raw or "Unnamed"


def convert_csv(csv_path, shorten=True, min_mhz=0.0, max_mhz=float("inf"),
                include_description=False):
    bookmarks = {}
    name_counts = {}

    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                freq_mhz = float(row["Frequency Output"])
            except (KeyError, ValueError):
                continue
            if freq_mhz <= 0 or freq_mhz < min_mhz or freq_mhz > max_mhz:
                continue

            freq_hz  = freq_mhz * 1_000_000.0
            mode     = MODE_MAP.get((row.get("Mode") or "").strip().upper(), 2)
            desc     = (row.get("Description") or "").strip()
            alpha    = (row.get("Alpha Tag")   or "").strip()

            name = short_name(alpha, desc) if shorten else (alpha or desc or f"{freq_mhz:.4f}")

            # Dedupe identical names
            if name in name_counts:
                name_counts[name] += 1
                unique = f"{name} ({name_counts[name]})"
            else:
                name_counts[name] = 1
                unique = name

            entry = {
                "bandwidth": DEFAULT_BW.get(mode, 12500.0),
                "frequency": freq_hz,
                "mode":      mode,
            }
            # Description is an extension used by channel_bank; SDR++ FM CRASHES on
            # unknown fields, so omit by default.
            if include_description and desc and desc != name:
                entry["description"] = desc

            bookmarks[unique] = entry

    return bookmarks


def main():
    ap = argparse.ArgumentParser(description="RadioReference CSV -> SDR++ bookmark JSON")
    ap.add_argument("inputs", nargs="+", help="One or more RR CSV files")
    ap.add_argument("-o", "--output", required=True, help="Output JSON path")
    ap.add_argument("--name", default=None, help="List name (single input only)")
    ap.add_argument("--no-shorten", action="store_true",
                    help="Keep full Alpha Tag (don't strip ARTCC prefixes)")
    ap.add_argument("--min-mhz", type=float, default=0.0,
                    help="Skip entries below this frequency (MHz)")
    ap.add_argument("--max-mhz", type=float, default=float("inf"),
                    help="Skip entries above this frequency (MHz) — e.g. 300 to drop UHF")
    args = ap.parse_args()

    shorten = not args.no_shorten

    if len(args.inputs) > 1:
        lists = {}
        for path in args.inputs:
            stem = os.path.splitext(os.path.basename(path))[0]
            bm = convert_csv(path, shorten=shorten,
                             min_mhz=args.min_mhz, max_mhz=args.max_mhz)
            if bm:
                lists[stem] = {"bookmarks": bm}
                print(f"  {stem}: {len(bm)} bookmarks", file=sys.stderr)
        out = {"lists": lists}
    else:
        path = args.inputs[0]
        bm = convert_csv(path, shorten=shorten,
                         min_mhz=args.min_mhz, max_mhz=args.max_mhz)
        print(f"  {len(bm)} bookmarks", file=sys.stderr)
        if args.name:
            # Wrap in frequency_manager_config.json structure with the given list name
            out = {"lists": {args.name: {"bookmarks": bm}}}
        else:
            # Single bookmark group (matches SDR++ "Export Group" format)
            out = {"bookmarks": bm}

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    print(f"Wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
