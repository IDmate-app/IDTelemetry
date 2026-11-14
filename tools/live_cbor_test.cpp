// Host test for src/live_cbor.h: encodes the three telegram kinds and prints them as hex.
// tools/live_cbor_test.py decodes the hex with an independent CBOR reader and checks the content.
//   g++ -std=c++17 -I src tools/live_cbor_test.cpp -o /tmp/live_cbor_test && /tmp/live_cbor_test
#include "live_cbor.h"

static void dump(const char* name, const uint8_t* b, size_t n) {
    printf("%s %zu ", name, n);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main() {
    uint8_t buf[200];
    LiveValues v;
    memset(&v, 0, sizeof(v));
    v.has_soc = true;      v.soc = 71.5f;
    v.has_range = true;    v.range = 312.0f;
    v.has_power = true;    v.power = -12.4f;
    v.has_speed = true;    v.speed = 87.0f;
    v.has_odometer = true; v.odometer = 23456.7f;
    v.has_charging = true; v.charging = false;
    v.has_dc = true;       v.dc = false;
    v.has_t_ext = true;    v.t_ext = 9.0f;
    v.has_t_batt = true;   v.t_batt = 21.0f;
    v.has_pos = true;      v.lat = 50.123456; v.lon = 12.654321;
    uint32_t ts = 1794492201;   // 2026-11-12T14:03:21Z
    dump("status", buf, live_encode_status(v, ts, 4711, buf, sizeof(buf)));
    dump("position", buf, live_encode_position(v, ts, 4712, buf, sizeof(buf)));
    dump("caps", buf, live_encode_caps(v, ts, 4713, 1, buf, sizeof(buf)));
    // only what is valid goes out
    LiveValues few;
    memset(&few, 0, sizeof(few));
    few.has_soc = true; few.soc = 20.0f;
    dump("few", buf, live_encode_status(few, ts, 1, buf, sizeof(buf)));
    LiveValues none;
    memset(&none, 0, sizeof(none));
    printf("none %zu\n", live_encode_status(none, ts, 1, buf, sizeof(buf)));
    printf("nopos %zu\n", live_encode_position(none, ts, 1, buf, sizeof(buf)));
    printf("small %zu\n", live_encode_status(v, ts, 1, buf, 40));     // buffer too small -> 0, never a cut telegram
    return 0;
}
