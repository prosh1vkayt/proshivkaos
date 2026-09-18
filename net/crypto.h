/* net/crypto.h — SHA-1, HMAC, PBKDF2/PRF для WPA2, AES-128 и развёртка ключа. */
#ifndef NET_CRYPTO_H
#define NET_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t fill; } sha1_ctx_t;
void sha1_init(sha1_ctx_t *c);
void sha1_update(sha1_ctx_t *c, const void *data, size_t n);
void sha1_final(sha1_ctx_t *c, uint8_t out[20]);

typedef struct { sha1_ctx_t inner, outer; } hmac_sha1_ctx_t;
void hmac_sha1_init(hmac_sha1_ctx_t *m, const uint8_t *key, size_t klen);
void hmac_sha1_update(hmac_sha1_ctx_t *m, const void *d, size_t n);
void hmac_sha1_final(hmac_sha1_ctx_t *m, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, size_t klen, const void *d, size_t n, uint8_t out[20]);

/* PMK из пароля и имени сети (PBKDF2-SHA1, 4096 оборотов, 32 байта). */
void wpa_psk(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);
/* PRF-SHA1 из IEEE 802.11: HMAC(key, label || 0 || data || i) подряд. */
void wpa_prf(const uint8_t *key, size_t klen, const char *label,
             const uint8_t *data, size_t dlen, uint8_t *out, size_t olen);

typedef struct { uint8_t rk[176]; } aes128_t;
void aes128_init(aes128_t *a, const uint8_t key[16]);
void aes128_encrypt(const aes128_t *a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const aes128_t *a, const uint8_t in[16], uint8_t out[16]);
/* RFC 3394. n — число 64-битных блоков ключа; 1 — целостность сошлась. */
int  aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out);

#endif
