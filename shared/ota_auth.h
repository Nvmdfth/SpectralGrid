// HMAC authentication for the hub->probe OTA command (PKT_OTA). Device-only (uses mbedtls).
// Enabled only when shared/ota_secret.h exists; otherwise OTA_HAVE_SECRET is 0 and no probe OTA code is active.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"

#if defined(ARDUINO) && __has_include("ota_secret.h")
#include "ota_secret.h"
#define OTA_HAVE_SECRET 1
#else
#define OTA_HAVE_SECRET 0
#endif

#if OTA_HAVE_SECRET
#include "mbedtls/md.h"
static inline void ota_compute_tag(const OtaCmd *c, uint8_t out16[16]) {
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)OTA_SECRET, strlen(OTA_SECRET),
                    (const uint8_t *)c, offsetof(OtaCmd, tag), full);
    memcpy(out16, full, 16);
}
static inline void cmd_compute_tag(const ProbeCmd *c, uint8_t out16[16]) {
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)OTA_SECRET, strlen(OTA_SECRET),
                    (const uint8_t *)c, offsetof(ProbeCmd, tag), full);
    memcpy(out16, full, 16);
}
static inline bool cmd_tag_ok(const ProbeCmd *c) {
    uint8_t t[16], d = 0;
    cmd_compute_tag(c, t);
    for (int i = 0; i < 16; i++) d |= (uint8_t)(t[i] ^ c->tag[i]);     // constant time
    return d == 0;
}
static inline bool ota_tag_ok(const OtaCmd *c) {
    uint8_t t[16], d = 0;
    ota_compute_tag(c, t);
    for (int i = 0; i < 16; i++) d |= (uint8_t)(t[i] ^ c->tag[i]);     // constant time
    return d == 0;
}
#endif
