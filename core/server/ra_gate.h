#ifndef RA_GATE_H
#define RA_GATE_H

/* Authenticated binary plugin-gate record shared by the Vita and PSP modules. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RA_GATE_MAGIC 0x31474152u /* "RAG1" */
#define RA_GATE_VERSION 1u
#define RA_GATE_MAX_SKEW_SEC 300u

#define RA_GATE_FLAG_RA 0x00000001u
#define RA_GATE_FLAG_HARDCORE 0x00000002u
#define RA_GATE_FLAG_SPECTATOR 0x00000004u
#define RA_GATE_FLAG_ENCORE 0x00000008u
#define RA_GATE_FLAG_DEV 0x00000010u

#define RA_GATE_STATUS_VALID 0x00000001u
#define RA_GATE_STATUS_HARDCORE 0x00000002u
#define RA_GATE_STATUS_TAINTED 0x00000004u
#define RA_GATE_STATUS_INVALID 0x00000008u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t flags;
  uint32_t boot_id;
  uint32_t rtc_sec;
  uint8_t nonce[16];
  uint8_t mac[16];
  uint32_t crc;
  uint32_t status;
} RaGateRecord;

/* Rotation point: replace these bytes for each public release. They are
 * intentionally binary, not a plaintext passphrase. */
static const uint8_t RA_GATE_KEY[32] = {
  0x6a, 0x19, 0xc4, 0x77, 0x2e, 0xb8, 0x51, 0x0d,
  0x93, 0xef, 0x24, 0x8a, 0x5c, 0x37, 0xd1, 0x6b,
  0x08, 0xbe, 0x45, 0xf2, 0x71, 0x9d, 0x3a, 0xc6,
  0x54, 0xe0, 0x8f, 0x2b, 0x97, 0x63, 0x1c, 0xda
};

/* ---- SHA-256 / HMAC-SHA256 (compact, portable) ---- */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  size_t datalen;
} ra_sha256_ctx;

#define RA_ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ra_sha256_transform(ra_sha256_ctx *ctx, const uint8_t data[64]) {
  static const uint32_t k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
  };
  uint32_t m[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t s0, s1, t1, t2;
  int i;

  for (i = 0; i < 16; ++i) {
    m[i] = ((uint32_t)data[i * 4] << 24) |
      ((uint32_t)data[i * 4 + 1] << 16) |
      ((uint32_t)data[i * 4 + 2] << 8) |
      ((uint32_t)data[i * 4 + 3]);
  }
  for (i = 16; i < 64; ++i) {
    s0 = RA_ROR32(m[i - 15], 7) ^ RA_ROR32(m[i - 15], 18) ^ (m[i - 15] >> 3);
    s1 = RA_ROR32(m[i - 2], 17) ^ RA_ROR32(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }

  a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
  e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

  for (i = 0; i < 64; ++i) {
    s1 = RA_ROR32(e, 6) ^ RA_ROR32(e, 11) ^ RA_ROR32(e, 25);
    t1 = h + s1 + ((e & f) ^ (~e & g)) + k[i] + m[i];
    s0 = RA_ROR32(a, 2) ^ RA_ROR32(a, 13) ^ RA_ROR32(a, 22);
    t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void ra_sha256_init(ra_sha256_ctx *ctx) {
  ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
  ctx->bitlen = 0;
  ctx->datalen = 0;
}

static void ra_sha256_update(ra_sha256_ctx *ctx, const uint8_t *data, size_t len) {
  size_t i;

  for (i = 0; i < len; ++i) {
    ctx->data[ctx->datalen++] = data[i];
    if (ctx->datalen == 64) {
      ra_sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void ra_sha256_final(ra_sha256_ctx *ctx, uint8_t hash[32]) {
  size_t i = ctx->datalen;

  if (ctx->datalen < 56) {
    ctx->data[i++] = 0x80;
    while (i < 56)
      ctx->data[i++] = 0x00;
  } else {
    ctx->data[i++] = 0x80;
    while (i < 64)
      ctx->data[i++] = 0x00;
    ra_sha256_transform(ctx, ctx->data);
    memset(ctx->data, 0, 56);
  }

  ctx->bitlen += (uint64_t)ctx->datalen * 8;
  ctx->data[63] = (uint8_t)(ctx->bitlen);
  ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
  ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
  ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
  ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
  ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
  ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
  ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
  ra_sha256_transform(ctx, ctx->data);

  for (i = 0; i < 4; ++i) {
    hash[i]      = (uint8_t)(ctx->state[0] >> (24 - i * 8));
    hash[i + 4]  = (uint8_t)(ctx->state[1] >> (24 - i * 8));
    hash[i + 8]  = (uint8_t)(ctx->state[2] >> (24 - i * 8));
    hash[i + 12] = (uint8_t)(ctx->state[3] >> (24 - i * 8));
    hash[i + 16] = (uint8_t)(ctx->state[4] >> (24 - i * 8));
    hash[i + 20] = (uint8_t)(ctx->state[5] >> (24 - i * 8));
    hash[i + 24] = (uint8_t)(ctx->state[6] >> (24 - i * 8));
    hash[i + 28] = (uint8_t)(ctx->state[7] >> (24 - i * 8));
  }
}

static void ra_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  ra_sha256_ctx ctx;
  ra_sha256_init(&ctx);
  ra_sha256_update(&ctx, data, len);
  ra_sha256_final(&ctx, out);
}

static void ra_hmac_sha256(const uint8_t *key, size_t keylen,
            const uint8_t *msg, size_t msglen,
            uint8_t out[32]) {
  uint8_t k[64];
  uint8_t ipad[64];
  uint8_t opad[64];
  uint8_t inner[32];
  ra_sha256_ctx ctx;
  size_t i;

  memset(k, 0, sizeof(k));
  if (keylen > sizeof(k))
    ra_sha256(key, keylen, k);
  else
    memcpy(k, key, keylen);

  for (i = 0; i < sizeof(k); ++i) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5c);
  }

  ra_sha256_init(&ctx);
  ra_sha256_update(&ctx, ipad, sizeof(ipad));
  ra_sha256_update(&ctx, msg, msglen);
  ra_sha256_final(&ctx, inner);

  ra_sha256_init(&ctx);
  ra_sha256_update(&ctx, opad, sizeof(opad));
  ra_sha256_update(&ctx, inner, sizeof(inner));
  ra_sha256_final(&ctx, out);
}

/* ---- Gate record helpers ---- */

/* A constant-time compare keeps the MAC from leaking how many leading bytes
 * matched. volatile stops the compiler from turning the loop back into an
 * early-exit memcmp. */
static int ra_gate_mac_equal(const uint8_t *left, const uint8_t *right,
                             size_t length) {
  volatile uint8_t diff = 0;
  size_t i;

  for (i = 0; i < length; ++i)
    diff = (uint8_t)(diff | (left[i] ^ right[i]));
  return diff == 0;
}

/* Authenticate every field except mac itself. Hashing only the bytes before
 * mac left crc and status unauthenticated, so either could be edited without
 * invalidating the record. The mac field is zeroed in the copy so the hash
 * covers the whole struct regardless of field order. */
static void ra_gate_mac(const RaGateRecord *record, uint8_t mac[32]) {
  RaGateRecord copy;

  memcpy(&copy, record, sizeof(copy));
  memset(copy.mac, 0, sizeof(copy.mac));
  ra_hmac_sha256(RA_GATE_KEY, sizeof(RA_GATE_KEY),
                 (const uint8_t*)&copy, sizeof(copy), mac);
}

static uint32_t ra_gate_crc(const RaGateRecord *record) {
  const uint8_t *bytes = (const uint8_t*)record;
  size_t length = offsetof(RaGateRecord, mac);
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

static inline void ra_gate_seal(RaGateRecord *record) {
  uint8_t mac[32];

  record->magic = RA_GATE_MAGIC;
  record->version = RA_GATE_VERSION;
  record->size = (uint16_t)sizeof(*record);
  record->crc = ra_gate_crc(record);
  ra_gate_mac(record, mac);
  memcpy(record->mac, mac, sizeof(record->mac));
}

static inline int ra_gate_verify(const RaGateRecord *record, uint32_t now_sec,
            uint32_t max_skew_sec) {
  uint8_t mac[32];
  uint32_t age;

  if (record == NULL)
    return 0;
  if (record->magic != RA_GATE_MAGIC ||
    record->version != RA_GATE_VERSION ||
    record->size != (uint16_t)sizeof(*record))
    return 0;
  if (record->crc != ra_gate_crc(record))
    return 0;

  ra_gate_mac(record, mac);
  if (!ra_gate_mac_equal(record->mac, mac, sizeof(record->mac)))
    return 0;

  age = now_sec > record->rtc_sec ? now_sec - record->rtc_sec
                  : record->rtc_sec - now_sec;
  return age <= max_skew_sec;
}

#endif /* RA_GATE_H */


