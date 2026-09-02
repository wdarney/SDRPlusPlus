#!/usr/bin/env python3
import argparse
import json
import shutil
from datetime import datetime
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(
        description="Enable the Brown DSD module in an SDR++ profile."
    )
    parser.add_argument(
        "--root",
        default="~/Library/Application Support/sdrpp",
        help="SDR++ profile root to update. Defaults to the original SDR++ profile.",
    )
    parser.add_argument(
        "--module-config-source",
        default="~/Library/Application Support/sdrpp-browndsd-test/ch_extravhf_decoder_config.json",
        help="Optional existing Brown DSD module config to copy if the profile lacks one.",
    )
    args = parser.parse_args()

    root = Path(args.root).expanduser()
    config_path = root / "config.json"
    if not config_path.exists():
        raise SystemExit(f"Missing SDR++ config: {config_path}")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup_path = config_path.with_name(f"config.json.codex-bak-before-browndsd-{stamp}")
    shutil.copy2(config_path, backup_path)

    with config_path.open() as f:
        config = json.load(f)

    instances = config.setdefault("moduleInstances", {})
    instances["Brown DSD"] = {
        "enabled": True,
        "module": "ch_extravhf_decoder",
    }

    with config_path.open("w") as f:
        json.dump(config, f, indent=4)
        f.write("\n")

    module_config = root / "ch_extravhf_decoder_config.json"
    if not module_config.exists():
        source = Path(args.module_config_source).expanduser()
        if source.exists():
            shutil.copy2(source, module_config)
        else:
            module_config.write_text("{}\n")

    print(f"Enabled Brown DSD in {config_path}")
    print(f"Config backup: {backup_path}")
    print(f"Module config: {module_config}")


if __name__ == "__main__":
    main()
