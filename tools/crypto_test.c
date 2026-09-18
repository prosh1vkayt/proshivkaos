/* tools/crypto_test.c — проверка net/crypto.c эталонными векторами.
 *   cc -O2 -Inet -o /tmp/ct tools/crypto_test.c net/crypto.c && /tmp/ct */
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int fails;

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
}

static void check(const char *name, const uint8_t *got, const char *want_hex, size_t n) {
    uint8_t want[128];
    hex(want_hex, want, n);
    int ok = memcmp(got, want, n) == 0;
    printf("%-28s %s\n", name, ok ? "OK" : "ОШИБКА");
    if (!ok) fails++;
}

int main(void) {
    uint8_t out[64];
    sha1_ctx_t c;
    sha1_init(&c); sha1_update(&c, "abc", 3); sha1_final(&c, out);
    check("SHA-1 abc (FIPS 180)", out, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);

    sha1_init(&c);
    for (int i = 0; i < 1000000; i++) sha1_update(&c, "a", 1);
    sha1_final(&c, out);
    check("SHA-1 миллион 'a'", out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", 20);

    uint8_t key[80];
    memset(key, 0x0b, 20);
    hmac_sha1(key, 20, "Hi There", 8, out);
    check("HMAC-SHA1 RFC 2202 #1", out, "b617318655057264e28bc0b6fb378c8ef146be00", 20);
    memset(key, 0xaa, 80);
    hmac_sha1(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    check("HMAC-SHA1 RFC 2202 #6", out, "aa4ae5e15272d00e95705637ce8a3b55ed402112", 20);

    wpa_psk("password", (const uint8_t *)"IEEE", 4, out);
    check("PSK 802.11 J.4 #1", out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);
    wpa_psk("ThisIsAPassword", (const uint8_t *)"ThisIsASSID", 11, out);
    check("PSK 802.11 J.4 #2", out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32);

    /* PRF: IEEE 802.11 J.3, «prefix», ключ 0x0b*20, данные "Hi There" */
    memset(key, 0x0b, 20);
    wpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
    check("PRF-512 802.11 J.3 #1", out,
          "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a", 64);

    aes128_t a;
    uint8_t k16[16], pt[16], ct[16];
    hex("000102030405060708090a0b0c0d0e0f", k16, 16);
    hex("00112233445566778899aabbccddeeff", pt, 16);
    aes128_init(&a, k16);
    aes128_encrypt(&a, pt, ct);
    check("AES-128 FIPS 197", ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
    aes128_decrypt(&a, ct, out);
    check("AES-128 обратно", out, "00112233445566778899aabbccddeeff", 16);

    uint8_t wrapped[24];
    hex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", wrapped, 24);
    int ok = aes_key_unwrap(k16, wrapped, 2, out);
    printf("%-28s %s\n", "RFC 3394 целостность", ok ? "OK" : "ОШИБКА");
    if (!ok) fails++;
    check("RFC 3394 4.1", out, "00112233445566778899aabbccddeeff", 16);

    printf(fails ? "\nНЕ ПРОШЛО: %d\n" : "\nВсё сошлось.\n", fails);
    return fails != 0;
}
