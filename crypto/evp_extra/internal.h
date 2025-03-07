// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0 OR ISC

#include <openssl/base.h>

typedef struct {
  // key is the concatenation of the private seed and public key. It is stored
  // as a single 64-bit array to allow passing to |ED25519_sign|. If
  // |has_private| is false, the first 32 bytes are uninitialized and the public
  // key is in the last 32 bytes.
  uint8_t key[64];
  char has_private;
} ED25519_KEY;

#define ED25519_PUBLIC_KEY_OFFSET 32

typedef struct {
  uint8_t pub[32];
  uint8_t priv[32];
  char has_private;
} X25519_KEY;

typedef struct {
  uint8_t pub[800];
  uint8_t priv[1632];
  uint8_t has_private;
} KYBER512_KEY;

extern const EVP_PKEY_ASN1_METHOD dsa_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD ec_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD rsa_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD rsa_pss_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD ed25519_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD x25519_asn1_meth;
extern const EVP_PKEY_ASN1_METHOD kyber512_asn1_meth;

extern const EVP_PKEY_METHOD ed25519_pkey_meth;
extern const EVP_PKEY_METHOD x25519_pkey_meth;
extern const EVP_PKEY_METHOD kyber512_pkey_meth;
extern const EVP_PKEY_METHOD hkdf_pkey_meth;

// Returns a reference to the list |non_fips_pkey_evp_methods|. The list has
// size |NON_FIPS_EVP_PKEY_METHODS|.
const EVP_PKEY_METHOD *const *AWSLC_non_fips_pkey_evp_methods(void);

// Returns a reference to the list |asn1_evp_pkey_methods|. The list has
// size |ASN1_EVP_PKEY_METHODS|.
const EVP_PKEY_ASN1_METHOD *const *AWSLC_non_fips_pkey_evp_asn1_methods(void);
