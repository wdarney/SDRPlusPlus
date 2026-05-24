#!/usr/bin/env python3
"""
Merge new bookmark lists into an existing SDR++ frequency_manager_config.json.
Preserves all existing lists; adds new ones. Refuses to overwrite an existing
list name unless --overwrite is passed.

Usage:
    python3 merge_lists.py \
        --existing ~/.../frequency_manager_config.json \
        --add-csv "ZNY ARTCC" ~/.../aid_2246_*.csv \
        --add-csv "ZBW ARTCC" ~/.../aid_2248_*.csv \
        --add-manual KBDL bradley.json \
        -o ~/Desktop/frequency_manager_config.merged.json
"""

import argparse
import json
import os
import sys

# Reuse the converter
sys.path.insert(0, os.path.dirname(__file__))
from rr_to_sdrpp import convert_csv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--existing", required=True, help="Current frequency_manager_config.json")
    ap.add_argument("--add-csv", nargs=2, action="append", default=[],
                    metavar=("LIST_NAME", "CSV_PATH"),
                    help="Add a RadioReference CSV as a new list")
    ap.add_argument("--add-manual", nargs=2, action="append", default=[],
                    metavar=("LIST_NAME", "JSON_PATH"),
                    help="Add a bookmark-group JSON ({bookmarks: {...}}) as a new list")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--overwrite", action="store_true",
                    help="Allow replacing existing lists with the same name")
    ap.add_argument("--min-mhz", type=float, default=0.0,
                    help="Drop entries below this frequency (MHz)")
    ap.add_argument("--max-mhz", type=float, default=float("inf"),
                    help="Drop entries above this frequency (MHz)")
    args = ap.parse_args()

    with open(args.existing) as f:
        cfg = json.load(f)
    if "lists" not in cfg:
        cfg["lists"] = {}

    existing_names = set(cfg["lists"].keys())

    def add_list(name, bookmarks):
        if name in existing_names and not args.overwrite:
            print(f"  SKIP '{name}' (already exists — use --overwrite to replace)", file=sys.stderr)
            return
        # SDR++ FM requires showOnWaterfall per list or it crashes on load
        cfg["lists"][name] = {"bookmarks": bookmarks, "showOnWaterfall": True}
        action = "REPLACED" if name in existing_names else "ADDED"
        print(f"  {action} '{name}': {len(bookmarks)} bookmarks", file=sys.stderr)

    # Ensure every existing list has showOnWaterfall too (safety)
    for lname, lst in cfg["lists"].items():
        if isinstance(lst, dict) and "showOnWaterfall" not in lst:
            lst["showOnWaterfall"] = True

    for list_name, csv_path in args.add_csv:
        bm = convert_csv(csv_path, shorten=True,
                         min_mhz=args.min_mhz, max_mhz=args.max_mhz)
        add_list(list_name, bm)

    def freq_ok(hz):
        mhz = hz / 1e6
        return args.min_mhz <= mhz <= args.max_mhz

    for list_name, json_path in args.add_manual:
        with open(json_path) as f:
            data = json.load(f)
        bm = data.get("bookmarks") or data
        bm = {n: e for n, e in bm.items() if freq_ok(e.get("frequency", 0))}
        add_list(list_name, bm)

    with open(args.output, "w") as f:
        json.dump(cfg, f, indent=4)

    print(f"\nWrote {args.output} ({len(cfg['lists'])} total lists)", file=sys.stderr)


if __name__ == "__main__":
    main()
