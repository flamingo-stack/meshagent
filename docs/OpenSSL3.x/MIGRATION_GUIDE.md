# OpenSSL 3.x Migration Guide

This guide provides detailed before/after examples for each type of cryptographic operation migrated from OpenSSL 1.1.1 to OpenSSL 3.x.

## Table of Contents
1. [Message Digest (Hash) Functions](#message-digest-hash-functions)
2. [RSA Public Key Encryption](#rsa-public-key-encryption)
3. [RSA Signing and Verification](#rsa-signing-and-verification)
4. [Error Handling](#error-handling)
5. [Common Patterns](#common-patterns)

---

## Message Digest (Hash) Functions

### SHA-256

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/sha.h>

char digest[32];
SHA256_CTX ctx;

SHA256_Init(&ctx);
SHA256_Update(&ctx, data, data_len);
SHA256_Final((unsigned char*)digest, &ctx);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

char digest[32];
EVP_MD_CTX *ctx = EVP_MD_CTX_new();

if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, data_len);
    EVP_DigestFinal_ex(ctx, (unsigned char*)digest, NULL);
    EVP_MD_CTX_free(ctx);
}
```

**Key Changes:**
- Context is heap-allocated via `EVP_MD_CTX_new()` instead of stack variable
- Must check for `NULL` (allocation can fail)
- Must call `EVP_MD_CTX_free()` to release memory
- Functions use `_ex` suffix variants

---

### SHA-384

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/sha.h>

char hash[48];
SHA512_CTX ctx;

SHA384_Init(&ctx);
SHA384_Update(&ctx, data, len);
SHA384_Final((unsigned char*)hash, &ctx);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

char hash[48];
EVP_MD_CTX *ctx = EVP_MD_CTX_new();

if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha384(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, (unsigned char*)hash, NULL);
    EVP_MD_CTX_free(ctx);
}
```

---

### SHA-512

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/sha.h>

char hash[64];
SHA512_CTX ctx;

SHA512_Init(&ctx);
SHA512_Update(&ctx, data, len);
SHA512_Final((unsigned char*)hash, &ctx);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

char hash[64];
EVP_MD_CTX *ctx = EVP_MD_CTX_new();

if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha512(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, (unsigned char*)hash, NULL);
    EVP_MD_CTX_free(ctx);
}
```

---

### SHA-1

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/sha.h>

unsigned char hash[20];
SHA_CTX ctx;

SHA1_Init(&ctx);
SHA1_Update(&ctx, data, len);
SHA1_Final(hash, &ctx);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

unsigned char hash[20];
EVP_MD_CTX *ctx = EVP_MD_CTX_new();

if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, NULL);
    EVP_MD_CTX_free(ctx);
}
```

**Note:** SHA-1 is cryptographically weak. Used here only for WebSocket protocol compatibility (RFC 6455).

---

### MD5

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/md5.h>

char hash[16];
MD5_CTX ctx;

MD5_Init(&ctx);
MD5_Update(&ctx, data, len);
MD5_Final((unsigned char*)hash, &ctx);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

char hash[16];
EVP_MD_CTX *ctx = EVP_MD_CTX_new();

if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, (unsigned char*)hash, NULL);
    EVP_MD_CTX_free(ctx);
}
```

**Note:** MD5 is cryptographically broken. Used here only for HTTP Digest Authentication (RFC 2617) compatibility.

---

## RSA Public Key Encryption

### RSA OAEP Encryption

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/rsa.h>
#include <openssl/pem.h>

RSA *rsa = PEM_read_RSA_PUBKEY(fp, NULL, NULL, NULL);
unsigned char encrypted[256];

int len = RSA_public_encrypt(
    plaintext_len,
    (unsigned char*)plaintext,
    encrypted,
    rsa,
    RSA_PKCS1_OAEP_PADDING
);

RSA_free(rsa);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>
#include <openssl/pem.h>

EVP_PKEY *pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
unsigned char encrypted[256];
size_t outlen = sizeof(encrypted);

EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
if (ctx != NULL) {
    if (EVP_PKEY_encrypt_init(ctx) > 0) {
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
        EVP_PKEY_encrypt(ctx, encrypted, &outlen,
                        (unsigned char*)plaintext, plaintext_len);
    }
    EVP_PKEY_CTX_free(ctx);
}

EVP_PKEY_free(pkey);
```

**Key Changes:**
- Use `EVP_PKEY` instead of `RSA*`
- Use `PEM_read_PUBKEY()` instead of `PEM_read_RSA_PUBKEY()`
- Encryption requires `EVP_PKEY_CTX`
- Padding is set via `EVP_PKEY_CTX_set_rsa_padding()`
- Output length is `size_t*` instead of return value

---

### RSA OAEP Decryption

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/rsa.h>
#include <openssl/pem.h>

RSA *rsa = PEM_read_RSAPrivateKey(fp, NULL, NULL, NULL);
unsigned char decrypted[256];

int len = RSA_private_decrypt(
    ciphertext_len,
    (unsigned char*)ciphertext,
    decrypted,
    rsa,
    RSA_PKCS1_OAEP_PADDING
);

RSA_free(rsa);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>
#include <openssl/pem.h>

EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
unsigned char decrypted[256];
size_t outlen = sizeof(decrypted);

EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
if (ctx != NULL) {
    if (EVP_PKEY_decrypt_init(ctx) > 0) {
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
        EVP_PKEY_decrypt(ctx, decrypted, &outlen,
                        (unsigned char*)ciphertext, ciphertext_len);
    }
    EVP_PKEY_CTX_free(ctx);
}

EVP_PKEY_free(pkey);
```

---

## RSA Signing and Verification

### RSA + SHA-384 Signing

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/rsa.h>
#include <openssl/sha.h>

// First compute SHA-384 hash
SHA512_CTX sha_ctx;
unsigned char hash[48];
SHA384_Init(&sha_ctx);
SHA384_Update(&sha_ctx, data, data_len);
SHA384_Final(hash, &sha_ctx);

// Then sign the hash
RSA *rsa = PEM_read_RSAPrivateKey(fp, NULL, NULL, NULL);
unsigned char signature[256];
unsigned int sig_len;

RSA_sign(NID_sha384, hash, 48, signature, &sig_len, rsa);
RSA_free(rsa);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
unsigned char signature[256];
size_t sig_len = sizeof(signature);

EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha384(), NULL, pkey) > 0) {
        EVP_DigestSignUpdate(ctx, data, data_len);
        EVP_DigestSignFinal(ctx, signature, &sig_len);
    }
    EVP_MD_CTX_free(ctx);
}

EVP_PKEY_free(pkey);
```

**Key Changes:**
- Combined hash + sign into single operation
- Uses `EVP_DigestSign*()` functions
- Automatically handles hash computation
- Single context for entire operation

---

### RSA + SHA-384 Verification

#### Before (OpenSSL 1.1.1)
```c
#include <openssl/rsa.h>
#include <openssl/sha.h>

// First compute SHA-384 hash
SHA512_CTX sha_ctx;
unsigned char hash[48];
SHA384_Init(&sha_ctx);
SHA384_Update(&sha_ctx, data, data_len);
SHA384_Final(hash, &sha_ctx);

// Then verify signature
RSA *rsa = PEM_read_RSA_PUBKEY(fp, NULL, NULL, NULL);
int result = RSA_verify(NID_sha384, hash, 48, signature, sig_len, rsa);
RSA_free(rsa);
```

#### After (OpenSSL 3.x)
```c
#include <openssl/evp.h>

EVP_PKEY *pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
int result = 0;

EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha384(), NULL, pkey) > 0) {
        EVP_DigestVerifyUpdate(ctx, data, data_len);
        result = EVP_DigestVerifyFinal(ctx, signature, sig_len);
    }
    EVP_MD_CTX_free(ctx);
}

EVP_PKEY_free(pkey);
// result > 0 means signature is valid
```

---

## Error Handling

### Memory Allocation Failures

Always check for `NULL` returns from `_new()` functions:

```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx == NULL) {
    // Handle allocation failure
    return -1;
}

// Use ctx...

EVP_MD_CTX_free(ctx);  // Safe even if ctx is NULL
```

### Operation Failures

Check return values from `_init()` and operation functions:

```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) <= 0) {
        // Initialization failed
        EVP_MD_CTX_free(ctx);
        return -1;
    }

    if (EVP_DigestUpdate(ctx, data, len) <= 0) {
        // Update failed
        EVP_MD_CTX_free(ctx);
        return -1;
    }

    if (EVP_DigestFinal_ex(ctx, digest, NULL) <= 0) {
        // Finalization failed
        EVP_MD_CTX_free(ctx);
        return -1;
    }

    EVP_MD_CTX_free(ctx);
}
```

---

## Common Patterns

### Pattern 1: Simple One-Shot Hash

When you just need a single hash with no streaming:

```c
// OpenSSL 3.x convenience function
unsigned char digest[32];
unsigned int len = 32;

EVP_Digest(data, data_len, digest, &len, EVP_sha256(), NULL);
```

This is equivalent to:
```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, data_len);
    EVP_DigestFinal_ex(ctx, digest, NULL);
    EVP_MD_CTX_free(ctx);
}
```

### Pattern 2: Streaming Hash (Multiple Updates)

For large data or data arriving in chunks:

```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    // Process data in chunks
    while (more_data) {
        EVP_DigestUpdate(ctx, chunk, chunk_len);
    }

    EVP_DigestFinal_ex(ctx, digest, NULL);
    EVP_MD_CTX_free(ctx);
}
```

### Pattern 3: Reusing Context

Contexts can be reset and reused:

```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
if (ctx != NULL) {
    // First hash
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data1, len1);
    EVP_DigestFinal_ex(ctx, digest1, NULL);

    // Reset and compute second hash
    EVP_MD_CTX_reset(ctx);  // Reset instead of free+new
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data2, len2);
    EVP_DigestFinal_ex(ctx, digest2, NULL);

    EVP_MD_CTX_free(ctx);
}
```

---

## Header File Changes

### Old Headers (OpenSSL 1.1.1)
```c
#include <openssl/sha.h>    // For SHA functions
#include <openssl/md5.h>    // For MD5 functions
#include <openssl/rsa.h>    // For RSA functions
```

### New Headers (OpenSSL 3.x)
```c
#include <openssl/evp.h>    // For all EVP functions (unified API)
#include <openssl/pem.h>    // For PEM key reading (if needed)
```

**Note:** The EVP header provides a unified interface for all cryptographic operations.

---

## Performance Considerations

1. **Context Reuse**: Reusing contexts (with `EVP_MD_CTX_reset()`) is more efficient than creating new ones
2. **One-Shot Operations**: For single-use hashes, `EVP_Digest()` is optimized
3. **Memory**: EVP contexts are heap-allocated, slightly more overhead than stack variables
4. **Optimization**: OpenSSL 3.x may use hardware acceleration automatically

---

## Migration Checklist

When migrating a function:

- [ ] Replace old hash context types with `EVP_MD_CTX*`
- [ ] Replace `_Init()` with `EVP_DigestInit_ex()`
- [ ] Replace `_Update()` with `EVP_DigestUpdate()`
- [ ] Replace `_Final()` with `EVP_DigestFinal_ex()`
- [ ] Add `EVP_MD_CTX_new()` at start
- [ ] Add `EVP_MD_CTX_free()` at end
- [ ] Add NULL checks after `_new()`
- [ ] Update includes to use `<openssl/evp.h>`
- [ ] Test with known-good test vectors

---

*Last updated: November 24, 2025*
