#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single decoder, called only by the decoder worker. No host signal handlers. */
typedef struct {
    uint32_t address;
    char type[32], flight[9];
    double lat, lon, seen, seen_pos, rssi, speed, track;
    int altitude, vertical_rate, ground, squawk, category;
    unsigned valid;
    uint64_t messages;
} adsb_aircraft;
enum { ADSB_POSITION=1, ADSB_ALTITUDE=2, ADSB_SPEED=4, ADSB_TRACK=8,
       ADSB_RATE=16, ADSB_SQUAWK=32, ADSB_FLIGHT=64 };
void adsb_decoder_init(double lat, double lon, int location_valid);
void adsb_decoder_destroy(void);
void adsb_decoder_iq(const uint8_t *iq, size_t bytes, uint64_t now_ms, int discontinuous);
int adsb_decoder_message(const uint8_t *message, size_t bytes, uint64_t now_ms);
size_t adsb_decoder_snapshot(adsb_aircraft *out, size_t capacity, uint64_t now_ms);
uint64_t adsb_decoder_messages(void);
#ifdef __cplusplus
}
#endif
