#!/usr/bin/env python3
"""Build a focused macOS test app. Does not modify the normal SDR++ profile."""
from pathlib import Path
import os
import json
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
os.chdir(repo)
build = Path("/private/tmp/sdrpp-adsb-build")
options = re.findall(r"option\((OPT_BUILD_\w+)", (repo / "CMakeLists.txt").read_text())
args = ["cmake", "-S", ".", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DUSE_BUNDLE_DEFAULTS=ON", "-DOPT_ADSB_TESTS=ON"]
args += [f"-D{option}=OFF" for option in options]
args += [f"-DOPT_BUILD_{name}=ON" for name in ["ADSB", "RADIO", "RTL_SDR_SOURCE", "AUDIO_SINK"]]
subprocess.run(args, check=True)
subprocess.run(["cmake", "--build", str(build), "--target", "sdrpp", "adsb", "radio",
                "rtl_sdr_source", "audio_sink", "adsb_decoder_test", "adsb_http_fixture", "-j8"], check=True)
subprocess.run([str(build / "misc_modules/adsb/adsb_decoder_test")], check=True)
subprocess.run([str(build / "misc_modules/adsb/adsb_http_fixture"), "misc_modules/adsb/web", "--receiver-check"], check=True)
subprocess.run(["python3", "misc_modules/adsb/tests/http_test.py", str(build / "misc_modules/adsb/adsb_http_fixture"), "misc_modules/adsb/web"], check=True)

stage = Path(tempfile.mkdtemp(prefix="sdrpp-adsb-package-", dir="/private/tmp"))
bundle = stage / "SDR++-ADSB-Test.app"
env = dict(os.environ, COPYFILE_DISABLE="1", SDRPP_APP_NAME="SDR++ADSBTest",
           SDRPP_DISPLAY_NAME="SDR++ ADS-B Test", SDRPP_BUNDLE_ID="org.sdrpp.adsbtest")
subprocess.run(["bash", "make_macos_bundle.sh", str(build), str(bundle)], env=env, check=True)
binary = bundle / "Contents/MacOS/sdrpp"
binary.rename(binary.with_name("sdrpp-bin"))
launcher = (repo / "macos/sdrpp_app_launcher.c").read_text()
old = '%s/Library/Application Support/sdrpp"'
assert launcher.count(old) == 1
launcher = launcher.replace(old, '%s/Library/Application Support/sdrpp-adsb-test"')
launcher_path = stage / "launcher.c"
launcher_path.write_text(launcher)
subprocess.run(["cc", "-Os", str(launcher_path), "-o", str(binary)], check=True)
for file in bundle.rglob("*"):
    if file.is_file():
        file.chmod(file.stat().st_mode | 0o200)
subprocess.run(["xattr", "-cr", str(bundle)], check=True)
subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(bundle)], check=True)
subprocess.run(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=True)
profile = Path.home() / "Library/Application Support/sdrpp-adsb-test"
profile.mkdir(parents=True, exist_ok=True)
config_path = profile / "config.json"
if not config_path.exists():
    config_path.write_text(json.dumps({
        "modulesDirectory": "../Plugins", "resourcesDirectory": "../Resources",
        "moduleInstances": {"ADS-B": {"module": "adsb", "enabled": True},
                            "RTL-SDR Source": {"module": "rtl_sdr_source", "enabled": True},
                            "Audio Sink": {"module": "audio_sink", "enabled": True},
                            "Radio": {"module": "radio", "enabled": True}},
        "menuElements": [{"name": "ADS-B", "open": True}, {"name": "Source", "open": False}],
        "source": "RTL-SDR", "autostart": False,
    }, indent=4))
print(f"\nTest app: {bundle}\nProfile: ~/Library/Application Support/sdrpp-adsb-test")
print("The dedicated test profile starts with an ADS-B module instance. Reception starts manually.")
