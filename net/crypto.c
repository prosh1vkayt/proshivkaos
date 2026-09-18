/* net/crypto.c — криптография WPA2-PSK.
 *
 * Шифрует и расшифровывает кадры сама прошивка Wi-Fi (CCMP в железе), нам
 * остаётся рукопожатие — а для него нужно немного:
 *
 *   SHA-1 и HMAC-SHA1   основа всего остального;
 *   PBKDF2-SHA1         пароль + имя сети -> PMK (4096 оборотов HMAC);
 *   PRF-SHA1            PMK + адреса + случайные числа -> PTK;
 *   AES-128             только ради развёртки ключа группы (RFC 3394),
 *                       которым точка присылает GTK в третьем сообщении.
 *
 * Всё написано по стандартам (FIPS 180, RFC 2104, RFC 2898, IEEE 802.11
 * 12.7.1.2, FIPS 197, RFC 3394) и проверено их же эталонными векторами —
 * tools/crypto_test.c собирает этот файл на компьютере и сверяет. */
#include "crypto.h"

/* ---------------- SHA-1 ---------------- */

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->fill = 0;
}

void sha1_update(sha1_ctx_t *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        for (size_t i = 0; i < take; i++) c->buf[c->fill + i] = p[i];
        c->fill += take; p += take; n -= take;
        if (c->fill == 64) { sha1_block(c->h, c->buf); c->fill = 0; }
    }
}

void sha1_final(sha1_ctx_t *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->fill != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* ---------------- HMAC-SHA1 ---------------- */

void hmac_sha1_init(hmac_sha1_ctx_t *m, const uint8_t *key, size_t klen) {
    uint8_t k[64] = { 0 };
    if (klen > 64) {
        sha1_ctx_t t; sha1_init(&t); sha1_update(&t, key, klen); sha1_final(&t, k);
    } else {
        for (size_t i = 0; i < klen; i++) k[i] = key[i];
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&m->inner); sha1_update(&m->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha1_init(&m->outer); sha1_update(&m->outer, pad, 64);
}

void hmac_sha1_update(hmac_sha1_ctx_t *m, const void *d, size_t n) { sha1_update(&m->inner, d, n); }

void hmac_sha1_final(hmac_sha1_ctx_t *m, uint8_t out[20]) {
    uint8_t ih[20];
    sha1_final(&m->inner, ih);
    sha1_update(&m->outer, ih, 20);
    sha1_final(&m->outer, out);
}

void hmac_sha1(const uint8_t *key, size_t klen, const void *d, size_t n, uint8_t out[20]) {
    hmac_sha1_ctx_t m;
    hmac_sha1_init(&m, key, klen);
    hmac_sha1_update(&m, d, n);
    hmac_sha1_final(&m, out);
}

/* ---------------- PBKDF2 и PRF (IEEE 802.11) ---------------- */

void wpa_psk(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]) {
    size_t plen = 0;
    while (pass[plen]) plen++;
    hmac_sha1_ctx_t base;
    hmac_sha1_init(&base, (const uint8_t *)pass, plen);
    for (int block = 1; block <= 2; block++) {
        uint8_t u[20], t[20];
        hmac_sha1_ctx_t m = base;
        uint8_t be[4] = { 0, 0, 0, (uint8_t)block };
        hmac_sha1_update(&m, ssid, ssid_len);
        hmac_sha1_update(&m, be, 4);
        hmac_sha1_final(&m, u);
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (int it = 1; it < 4096; it++) {
            m = base;
            hmac_sha1_update(&m, u, 20);
            hmac_sha1_final(&m, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        for (int i = 0; i < 20 && (block - 1) * 20 + i < 32; i++) pmk[(block - 1) * 20 + i] = t[i];
    }
}

void wpa_prf(const uint8_t *key, size_t klen, const char *label,
             const uint8_t *data, size_t dlen, uint8_t *out, size_t olen) {
    size_t lablen = 0;
    while (label[lablen]) lablen++;
    uint8_t zero = 0;
    for (uint8_t i = 0; olen; i++) {
        uint8_t h[20];
        hmac_sha1_ctx_t m;
        hmac_sha1_init(&m, key, klen);
        hmac_sha1_update(&m, label, lablen);
        hmac_sha1_update(&m, &zero, 1);
        hmac_sha1_update(&m, data, dlen);
        hmac_sha1_update(&m, &i, 1);
        hmac_sha1_final(&m, h);
        size_t n = olen < 20 ? olen : 20;
        for (size_t k = 0; k < n; k++) out[k] = h[k];
        out += n; olen -= n;
    }
}

/* ---------------- AES-128 ---------------- */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t inv_sbox[256];
static int g_inv_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

void aes128_init(aes128_t *a, const uint8_t key[16]) {
    if (!g_inv_ready) {
        for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
        g_inv_ready = 1;
    }
    uint8_t *rk = a->rk;
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}

void aes128_encrypt(const aes128_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        /* ShiftRows: столбцы по 4 байта, строка i сдвигается на i */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) s[4 * c + row] = t[4 * ((c + row) % 4) + row];
        if (r != 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = s + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                q[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                q[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                q[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

void aes128_decrypt(const aes128_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * ((c + row) % 4) + row] = s[4 * c + row];
        for (int i = 0; i < 16; i++) s[i] = inv_sbox[t[i]] ^ a->rk[16 * r + i];
        if (r != 0)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = s + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                q[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                q[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                q[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* RFC 3394: развернуть n 64-битных блоков. in — (n + 1) * 8 байт. */
int aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out) {
    aes128_t a;
    aes128_init(&a, kek);
    uint8_t A[8];
    for (int i = 0; i < 8; i++) A[i] = in[i];
    for (size_t i = 0; i < n * 8; i++) out[i] = in[8 + i];
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            uint8_t b[16];
            for (int k = 0; k < 8; k++) b[k] = A[k] ^ (uint8_t)(t >> (56 - 8 * k));
            for (int k = 0; k < 8; k++) b[8 + k] = out[(i - 1) * 8 + k];
            aes128_decrypt(&a, b, b);
            for (int k = 0; k < 8; k++) A[k] = b[k];
            for (int k = 0; k < 8; k++) out[(i - 1) * 8 + k] = b[8 + k];
        }
    }
    for (int k = 0; k < 8; k++) if (A[k] != 0xA6) return 0;
    return 1;
}
