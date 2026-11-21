/* Host test: ccm.c against OpenSSL's AES-128-CCM.
 *   gcc -O2 -I../src ccm_test.c ../src/ccm.c -lcrypto && ./a.out */
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ccm.h"

static void aes_openssl(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n;
    EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_EncryptUpdate(c, out, &n, in, 16);
    EVP_CIPHER_CTX_free(c);
}

static void ref_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                        const uint8_t *msg, int len, uint8_t *out, uint8_t *tag) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n;
    EVP_EncryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_IVLEN, CCM_NONCE_LEN, NULL);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_TAG, CCM_MIC_LEN, NULL);
    EVP_EncryptInit_ex(c, NULL, NULL, key, nonce);
    EVP_EncryptUpdate(c, NULL, &n, NULL, len);
    if (aad_len) EVP_EncryptUpdate(c, NULL, &n, aad, aad_len);
    EVP_EncryptUpdate(c, out, &n, msg, len);
    EVP_EncryptFinal_ex(c, out + n, &n);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_GET_TAG, CCM_MIC_LEN, tag);
    EVP_CIPHER_CTX_free(c);
}

int main(void) {
    int failures = 0;
    srand(1);
    for (int t = 0; t < 2000; t++) {
        uint8_t key[16], nonce[13], aad[40], msg[64], ref[64], ref_tag[4], mine[64], mic[4];
        int aad_len = rand() % 20, len = 1 + rand() % 40;
        for (int i = 0; i < 16; i++) key[i] = rand();
        for (int i = 0; i < 13; i++) nonce[i] = rand();
        for (int i = 0; i < aad_len; i++) aad[i] = rand();
        for (int i = 0; i < len; i++) msg[i] = rand();
        ref_encrypt(key, nonce, aad, aad_len, msg, len, ref, ref_tag);
        memcpy(mine, msg, len);
        ccm_encrypt(aes_openssl, key, nonce, aad, aad_len, mine, len, mic);
        if (memcmp(mine, ref, len) || memcmp(mic, ref_tag, 4)) {
            failures++;
            continue;
        }
        if (!ccm_decrypt(aes_openssl, key, nonce, aad, aad_len, mine, len, mic) ||
            memcmp(mine, msg, len)) {
            failures++;
            continue;
        }
        mic[t % 4] ^= 1 << (t % 8); /* tampered MIC must fail */
        memcpy(mine, ref, len);
        if (ccm_decrypt(aes_openssl, key, nonce, aad, aad_len, mine, len, mic)) failures++;
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}
