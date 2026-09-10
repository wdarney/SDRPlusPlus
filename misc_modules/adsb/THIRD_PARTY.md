# Bundled sources

- dump1090: https://github.com/flightaware/dump1090, commit `0339a57b89cd6e61856cbb13ae342c31ae7be5ac`. License retained beside sources.
- tar1090: https://github.com/wiedehopf/tar1090, commit `115e40e6968eb6cdeb9e5ed7bdcdd1a2234ed2a2`. License retained beside sources.

Only decoder/tracker components of dump1090 are compiled. Its executable, network server, device handling, and signal handlers are excluded. tar1090 assets are unmodified except config.js, which selects local defaults. Aircraft metadata databases and long-term trace archives are not bundled.
