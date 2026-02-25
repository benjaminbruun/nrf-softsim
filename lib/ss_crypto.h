#ifndef SS_CRYPTO_H
#define SS_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#include <onomondo/softsim/log.h>
/* AES_BLOCKSIZE, enum enc_algorithm, and the five ss_utils_* crypto function
 * declarations (ss_utils_aes_decrypt/encrypt, ss_utils_3des_decrypt/encrypt,
 * ss_utils_ota_calc_cc) are all provided by onomondo-uicc via this include. */
#include <onomondo/softsim/crypto.h>

#define KMU_KEY_SIZE 16 /* AES-128 key size in bytes */

/**
 * @brief Key identifiers for KMU slots
 *
 * TODO: Make configurable
 */
enum key_identifier_base {
	KEY_ID_KI = 10,
	KEY_ID_KIC = 11,
	KEY_ID_KID = 12,
	KEY_ID_UNKNOWN = 13
};

/**
 * @brief Setup key in KMU for encryption/decryption/CC calculation
 *
 * @param key Pointer to key data
 * @param key_len Length of key data
 * @param key_id Key identifier (resolves into a KMU slot)
 *
 * @return int 0 on success, negative error code otherwise
 */
int ss_utils_setup_key(size_t key_len, uint8_t key[static key_len],
		       enum key_identifier_base key_id);

/**
 * @brief Check if a key exists in KMU
 *
 * Can be used to check SoftSIM provisioning status
 *
 * @param key_id Key identifier (resolves into a KMU slot)
 *
 * @return int 1 if key exists, 0 otherwise
 */
int ss_utils_check_key_existence(enum key_identifier_base key_id);

/**
 * @brief Perform AES-128 block encryption
 *
 * @param key Pointer to AES key
 * @param in Pointer to input plaintext block
 * @param out Pointer to output ciphertext block
 *
 * @return int 0 on success, negative error code otherwise
 */
int aes_128_encrypt_block(const uint8_t *key, const uint8_t *in, uint8_t *out);

#endif /* SS_CRYPTO_H */
