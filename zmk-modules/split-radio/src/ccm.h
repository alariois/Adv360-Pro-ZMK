/*
 * AES-128-CCM (RFC 3610) with a 13-byte nonce (L = 2) and a 4-byte MIC,
 * on top of any AES-128 block encryption (the nRF52840 ECB peripheral in
 * the firmware, OpenSSL in tests/ccm_test.c).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCM_NONCE_LEN 13
#define CCM_MIC_LEN 4

/* out = AES-128(key, in), 16 bytes each */
typedef void (*ccm_block_fn)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* Encrypts msg in place and writes the MIC. aad and msg up to 255 bytes. */
void ccm_encrypt(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, uint8_t *msg, size_t msg_len,
                 uint8_t mic[CCM_MIC_LEN]);

/* Decrypts msg in place; false (and msg garbage) if the MIC doesn't match. */
bool ccm_decrypt(ccm_block_fn aes, const uint8_t key[16], const uint8_t nonce[CCM_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, uint8_t *msg, size_t msg_len,
                 const uint8_t mic[CCM_MIC_LEN]);
