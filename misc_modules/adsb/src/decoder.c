#include "decoder.h"
#include "dump1090.h"

struct _Modes Modes;
enum { OVERLAP = 320, MAX_SAMPLES = 131072 };
static uint16_t magnitude[MAX_SAMPLES + OVERLAP];
static uint16_t magnitude_lut[65536], log_lut[65536];
static uint64_t sample_clock;
static int first_buffer;

/* These upstream facilities are deliberately not part of the embedded decoder. */
void modesQueueOutput(struct modesMessage *mm, struct aircraft *a) { (void)mm; (void)a; }
void adaptive_update(uint16_t *buf, unsigned length, struct modesMessage *mm) {
    (void)buf; (void)length; (void)mm;
}

void adsb_decoder_destroy(void) {
    while (Modes.aircrafts) {
        struct aircraft *a = Modes.aircrafts;
        Modes.aircrafts = a->next;
        free(a);
    }
}

void adsb_decoder_init(double lat, double lon, int location_valid) {
    adsb_decoder_destroy();
    memset(&Modes, 0, sizeof(Modes));
    Modes.quiet = 1;
    Modes.sample_rate = 2400000;
    Modes.maxRange = 1852 * 300;
    Modes.fUserLat = lat;
    Modes.fUserLon = lon;
    if (location_valid) Modes.bUserFlags = MODES_USER_LATLON_VALID;
    Modes.log10lut = log_lut;
    for (unsigned i = 0; i < 65536; ++i) {
        double re = ((int)(i & 255) - 127.5) / 127.5;
        double im = ((int)(i >> 8) - 127.5) / 127.5;
        magnitude_lut[i] = (uint16_t)round(fmin(1.0, sqrt(re*re + im*im)) * 65535);
        log_lut[i] = i ? (uint16_t)round(100 * log10(i)) : 0;
    }
    /* CRC correction off: do not create tracks by repairing noisy frames. */
    modesChecksumInit(0);
    icaoFilterInit();
    modeACInit();
    memset(magnitude, 0, sizeof(magnitude));
    sample_clock = 0;
    first_buffer = 1;
}

void adsb_decoder_iq(const uint8_t *iq, size_t bytes, uint64_t now_ms, int discontinuous) {
    size_t samples = bytes / 2;
    if (samples < OVERLAP || samples > MAX_SAMPLES) return;
    if (first_buffer || discontinuous) memset(magnitude, 0, OVERLAP * sizeof(uint16_t));
    double power = 0;
    for (size_t i = 0; i < samples; ++i) {
        uint16_t m = magnitude_lut[iq[2*i] | (iq[2*i+1] << 8)];
        magnitude[OVERLAP+i] = m;
        power += (double)m*m / (65535.0*65535.0);
    }
    struct mag_buf buf = {0};
    buf.data = magnitude;
    buf.totalLength = MAX_SAMPLES + OVERLAP;
    buf.validLength = samples + OVERLAP;
    buf.overlap = OVERLAP;
    buf.sampleTimestamp = sample_clock;
    uint64_t duration = (samples + OVERLAP) * 1000 / 2400000;
    buf.sysTimestamp = now_ms > duration ? now_ms - duration : 0;
    buf.flags = (first_buffer || discontinuous) ? MAGBUF_DISCONTINUOUS : 0;
    buf.mean_power = power / samples;
    demodulate2400(&buf);
    memmove(magnitude, magnitude + samples, OVERLAP * sizeof(uint16_t));
    sample_clock += samples * 5;
    first_buffer = 0;
}

int adsb_decoder_message(const uint8_t *message, size_t bytes, uint64_t now_ms) {
    if (bytes != 7 && bytes != 14) return 0;
    if ((size_t)modesMessageLenByType(message[0] >> 3) != bytes * 8) return 0;
    struct modesMessage mm = {0};
    unsigned char copy[14] = {0};
    memcpy(copy, message, bytes);
    mm.sysTimestampMsg = now_ms;
    mm.signalLevel = 0.01;
    if (decodeModesMessage(&mm, copy) < 0) return 0;
    useModesMessage(&mm);
    return 1;
}

size_t adsb_decoder_snapshot(adsb_aircraft *out, size_t capacity, uint64_t now_ms) {
    _messageNow = now_ms;
    trackPeriodicUpdate();
    icaoFilterExpire();
    /* Limit retained track state as well as the JSON output. New tracks are
       inserted at the head by upstream; discard the oldest tail on overflow. */
    struct aircraft **link = &Modes.aircrafts;
    for (size_t retained = 0; *link; ++retained) {
        if (retained < 4096) link = &(*link)->next;
        else { struct aircraft *old = *link; *link = old->next; free(old); }
    }
    size_t count = 0;
    for (struct aircraft *a = Modes.aircrafts; a && count < capacity; a = a->next) {
        if (!a->reliable || now_ms < a->seen || now_ms - a->seen > 60000) continue;
        adsb_aircraft *o = &out[count++];
        memset(o, 0, sizeof(*o));
        o->address = a->addr;
        static const char *types[] = {"adsb_icao", "adsb_icao_nt", "adsr_icao", "tisb_icao",
            "adsb_other", "adsr_other", "tisb_trackfile", "tisb_other", "mode_s", "unknown"};
        snprintf(o->type, sizeof(o->type), "%s", types[a->addrtype <= ADDR_UNKNOWN ? a->addrtype : ADDR_UNKNOWN]);
        o->messages = a->messages;
        o->seen = (now_ms - a->seen) / 1000.0;
        o->category = a->category;
        double power = 0;
        for (int i=0; i<8; ++i) power += a->signalLevel[i];
        o->rssi = 10 * log10(fmax(1e-12, power / 8));
        if (trackDataValid(&a->position_valid)) {
            o->valid |= ADSB_POSITION;
            o->lat = a->lat; o->lon = a->lon;
            o->seen_pos = (now_ms - a->position_valid.updated) / 1000.0;
        }
        o->ground = trackDataValid(&a->airground_valid) && a->airground == AG_GROUND;
        if (trackDataValid(&a->altitude_baro_valid)) { o->valid |= ADSB_ALTITUDE; o->altitude=a->altitude_baro; }
        if (trackDataValid(&a->gs_valid)) { o->valid |= ADSB_SPEED; o->speed=a->gs; }
        if (trackDataValid(&a->track_valid)) { o->valid |= ADSB_TRACK; o->track=a->track; }
        if (trackDataValid(&a->baro_rate_valid)) { o->valid |= ADSB_RATE; o->vertical_rate=a->baro_rate; }
        if (trackDataValid(&a->squawk_valid)) { o->valid |= ADSB_SQUAWK; o->squawk=a->squawk; }
        if (trackDataValid(&a->callsign_valid)) { o->valid |= ADSB_FLIGHT; memcpy(o->flight,a->callsign,9); }
    }
    return count;
}
uint64_t adsb_decoder_messages(void) { return Modes.stats_current.messages_total; }
