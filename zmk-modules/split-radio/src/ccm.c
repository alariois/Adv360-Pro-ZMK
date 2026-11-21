/*
 * AES-128-CCM, see ccm.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ccm.h"

#define L 2 /* length field bytes: 15 - nonce length */

static void xor_block(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] ^= src[i];
    }
}

/* CBC-MAC over B0 | encoded aad | msg (plaintext) -> tag (16 bytes) */
static void cbc_mac(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                    const uint8_t *aad, size_t aad_len, const uint8_t *msg, size_t msg_len,
                    uint8_t tag[16]) {
    uint8_t block[16];

    block[0] = (aad_len ? 0x40 : 0) | (((CCM_MIC_LEN - 2) / 2) << 3) | (L - 1);
    memcpy(&block[1], nonce, CCM_NONCE_LEN);
    block[14] = (uint8_t)(msg_len >> 8);
    block[15] = (uint8_t)msg_len;
    aes(key, block, tag);

    if (aad_len) {
        /* 2-byte length prefix, then aad, zero padded to blocks */
        uint8_t buf[16] = {(uint8_t)(aad_len >> 8), (uint8_t)aad_len};
        size_t fill = 2;
        for (size_t i = 0; i < aad_len; i++) {
            buf[fill++] = aad[i];
            if (fill == 16) {
                xor_block(tag, buf, 16);
                aes(key, tag, tag);
                memset(buf, 0, 16);
                fill = 0;
            }
        }
        if (fill) {
            xor_block(tag, buf, 16);
            aes(key, tag, tag);
        }
    }
    for (size_t off = 0; off < msg_len; off += 16) {
        size_t n = msg_len - off < 16 ? msg_len - off : 16;
        xor_block(tag, &msg[off], n);
        aes(key, tag, tag);
    }
}

/* msg ^= keystream A1, A2, ...; s0 = E(A0) */
static void ctr(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                uint8_t *msg, size_t msg_len, uint8_t s0[16]) {
    uint8_t a[16], s[16];
    a[0] = L - 1;
    memcpy(&a[1], nonce, CCM_NONCE_LEN);
    a[14] = a[15] = 0;
    aes(key, a, s0);
    for (size_t off = 0, i = 1; off < msg_len; off += 16, i++) {
        a[14] = (uint8_t)(i >> 8);
        a[15] = (uint8_t)i;
        aes(key, a, s);
        xor_block(&msg[off], s, msg_len - off < 16 ? msg_len - off : 16);
    }
}

void ccm_encrypt(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, uint8_t *msg, size_t msg_len,
                 uint8_t mic[CCM_MIC_LEN]) {
    uint8_t tag[16], s0[16];
    cbc_mac(aes, key, nonce, aad, aad_len, msg, msg_len, tag);
    ctr(aes, key, nonce, msg, msg_len, s0);
    for (int i = 0; i < CCM_MIC_LEN; i++) {
        mic[i] = tag[i] ^ s0[i];
    }
}

bool ccm_decrypt(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, uint8_t *msg, size_t msg_len,
                 const uint8_t mic[CCM_MIC_LEN]) {
    uint8_t tag[16], s0[16];
    uint8_t diff = 0;
    ctr(aes, key, nonce, msg, msg_len, s0);
    cbc_mac(aes, key, nonce, aad, aad_len, msg, msg_len, tag);
    for (int i = 0; i < CCM_MIC_LEN; i++) {
        diff |= mic[i] ^ tag[i] ^ s0[i]; /* constant time */
    }
    return diff == 0;
}
