/* test_ssid_clamp.c -- regression guard for fake-ultrareview Finding 2.
 *
 * The SSID-copy loops in libs/ClipBoy/src/WiFiScan.cpp read
 * snifferPacket->payload[38 .. 38+CB_SSID_LEN-1]. This proves CB_SSID_LEN can
 * NEVER return a value that makes that read exceed the captured frame length
 * (len = rx_ctrl.sig_len), for ANY attacker byte payload[37] (0..255) and any len.
 *
 * KEEP THIS MACRO IDENTICAL to the one in WiFiScan.cpp.
 *   build+run:  gcc scripts/tests/test_ssid_clamp.c -o /tmp/ssidtest && /tmp/ssidtest
 */
#include <stdio.h>
#include <string.h>

#define CB_SSID_LEN(pkt, len) ((len) > 37 ? ((38 + (pkt)->payload[37] > (len)) ? ((len) > 38 ? (len) - 38 : 0) : (int)(pkt)->payload[37]) : 0)

typedef struct { unsigned char payload[512]; } pkt_t;

int main(void) {
    int fails = 0, checked = 0;
    /* Exhaustive: every attacker length byte (0..255) x a spread of frame lengths,
     * including tiny/degenerate frames. */
    int lens[] = {0, 1, 37, 38, 39, 40, 45, 70, 100, 293, 300, 400};
    for (unsigned b = 0; b <= 255; b++) {
        for (unsigned li = 0; li < sizeof(lens)/sizeof(lens[0]); li++) {
            pkt_t p; memset(&p, 0xAA, sizeof(p));
            p.payload[37] = (unsigned char)b;
            int len = lens[li];
            int n = CB_SSID_LEN(&p, len);
            /* the loop reads indices 38 .. 38+n-1 -> highest index touched = 37+n */
            int over = (n < 0) || (n > 0 && (38 + n) > len);
            /* correctness: a LEGIT SSID (that fits: 38+b <= len) must NOT be truncated */
            int legit_fits = (len > 37) && (38 + (int)b <= len);
            int truncated_legit = legit_fits && (n != (int)b);
            if (over)  { fails++; printf("  OOB: payload[37]=%u len=%d -> n=%d (reads to idx %d)\n", b, len, n, 37+n); }
            if (truncated_legit) { fails++; printf("  TRUNCATED legit SSID: payload[37]=%u len=%d -> n=%d\n", b, len, n); }
            checked++;
        }
    }
    /* Spot-print a few representative cases */
    printf("representative cases (payload[37], len -> clamped):\n");
    struct { int b, len; } ex[] = {{0xFF,40},{0xFF,300},{32,70},{33,70},{5,40},{50,30},{0,10}};
    for (unsigned i=0;i<sizeof(ex)/sizeof(ex[0]);i++){
        pkt_t p; memset(&p,0xAA,sizeof(p)); p.payload[37]=(unsigned char)ex[i].b;
        int n=CB_SSID_LEN(&p, ex[i].len);
        printf("  0x%02X (%3d), len=%3d -> %3d\n", ex[i].b, ex[i].b, ex[i].len, n);
    }
    printf("\n%d cases checked. %s\n", checked, fails ? "FAIL" : "PASS: no OOB read, no legit SSID truncated, for any attacker byte");
    return fails ? 1 : 0;
}
