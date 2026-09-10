# Bundled sources

- dump1090: https://github.com/flightaware/dump1090, commit `0339a57b89cd6e61856cbb13ae342c31ae7be5ac`. License retained beside sources.
- tar1090: https://github.com/wiedehopf/tar1090, commit `115e40e6968eb6cdeb9e5ed7bdcdd1a2234ed2a2`. License retained beside sources.

Only decoder/tracker components of dump1090 are compiled. Its executable, network server, device handling, and signal handlers are excluded. tar1090 assets are unmodified except config.js, which selects local defaults. Aircraft metadata databases and long-term trace archives are not bundled.

## Natural Earth basic offline map

`web/basic-world.geojson` is the 1:50m admin-0 countries dataset from
https://github.com/nvkelso/natural-earth-vector at
`ca96624a56bd078437bca8184e78163e5039ad19` (`geojson/ne_50m_admin_0_countries.geojson`).
Country properties are reduced to English names; geometry is unchanged.
The same pinned repository supplies `geojson/ne_50m_populated_places_simple.geojson`;
city names, point geometry, and minimum zoom levels are retained.
Natural Earth data is public domain: https://www.naturalearthdata.com/about/terms-of-use/.
The local `sdrpp_basic_map.js` layer, HTML script inclusion, and layer registration
are SDR++ integration changes. No remote map tiles are bundled or downloaded.
