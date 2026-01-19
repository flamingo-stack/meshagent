# Files Changed in OpenSSL 3.x Migration

This document lists all files modified during the OpenSSL 1.1.1 to 3.x migration, with descriptions of changes made.

## Summary

**Total Files Modified:** 8
**Total Lines Changed:** 542 insertions(+), 329 deletions(-)
**Warnings Eliminated:** 65+ OpenSSL deprecation warnings

---

## Modified Files

### 1. microstack/ILibCrypto.c

**Purpose:** Core cryptographic utility library

**Changes:**
- Migrated SHA-256, SHA-384, SHA-512 hash functions to EVP API
- Migrated RSA public key encryption (PKCS1 OAEP padding)
- Migrated RSA private key decryption (PKCS1 OAEP padding)
- Updated RSA key generation to use EVP_PKEY_CTX
- Removed unused `RSA *rsa` variable (cleanup commit)

**Functions Modified:**
- `util_sha256()` - SHA-256 hashing
- `util_sha384()` - SHA-384 hashing (used for file integrity)
- `util_keyhash()` - SHA-384 key fingerprinting
- `util_encrypt()` - RSA public key encryption
- `util_decrypt()` - RSA private key decryption
- `util_mkCert()` / `util_mkCertEx()` - Certificate generation (RSA key gen)

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
// <openssl/rsa.h>
```

**Lines Changed:** ~150 insertions, ~80 deletions

**Warnings Eliminated:** 15+ deprecation warnings

---

### 2. microstack/ILibWebServer.c

**Purpose:** HTTP/HTTPS web server implementation

**Changes:**
- Migrated MD5 hash functions for HTTP Digest Authentication (RFC 2617)
- Updated MD5 context handling for authentication flow

**Functions Modified:**
- `ILibWebServer_Digest_GenerateNonce()` - MD5-based nonce generation
- `ILibWebServer_StreamBody_Identify()` - HTTP authentication hash computation

**Authentication Flow:**
HTTP Digest Authentication requires three sequential MD5 hashes:
1. `MD5(username:realm:password)` - HA1
2. `MD5(method:uri)` - HA2
3. `MD5(HA1:nonce:HA2)` - Response hash

All three now use EVP_MD_CTX API.

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/md5.h>
```

**Lines Changed:** ~40 insertions, ~25 deletions

**Warnings Eliminated:** 3 deprecation warnings

---

### 3. microstack/ILibSimpleDataStore.c

**Purpose:** Simple key-value data store with integrity checking

**Changes:**
- Migrated SHA-384 hash function for data integrity verification
- Updated hash computation for stored data blocks

**Functions Modified:**
- `ILibSimpleDataStore_Get()` - Data retrieval with integrity check
- Internal hash computation functions

**Use Case:**
The data store computes SHA-384 hashes of stored values to detect corruption or tampering. This is critical for agent configuration persistence.

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
```

**Lines Changed:** ~30 insertions, ~20 deletions

**Warnings Eliminated:** 2 deprecation warnings

---

### 4. microscript/ILibDuktape_HttpStream.c

**Purpose:** JavaScript HTTP client bindings (Duktape engine)

**Changes:**
- Migrated SHA-1 hash function for WebSocket handshake (RFC 6455)
- Updated WebSocket Sec-WebSocket-Accept header computation

**Functions Modified:**
- `ILibDuktape_HttpStream_PerformWebSocketUpgrade()` - WebSocket handshake

**WebSocket Protocol:**
Per RFC 6455, the server must compute:
```
Sec-WebSocket-Accept = Base64(SHA1(Sec-WebSocket-Key + GUID))
```
where GUID = `"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"`

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
```

**Lines Changed:** ~25 insertions, ~15 deletions

**Warnings Eliminated:** 1 deprecation warning

**Note:** SHA-1 is cryptographically weak but required for WebSocket protocol compatibility.

---

### 5. microscript/ILibDuktape_SHA256.c

**Purpose:** JavaScript cryptographic API bindings (Duktape engine)

**Changes:**
- Migrated ALL hash functions: MD5, SHA-1, SHA-256, SHA-384, SHA-512
- Migrated RSA signing and verification operations
- Updated streaming hash operations for JavaScript crypto API

**Functions Modified:**
- `ILibDuktape_crypto_createHash()` - Hash object creation
- `ILibDuktape_crypto_update()` - Streaming hash updates
- `ILibDuktape_crypto_digest()` - Hash finalization
- `ILibDuktape_crypto_createHmac()` - HMAC creation (Note: HMAC itself not yet migrated)
- `ILibDuktape_crypto_sign()` - RSA+SHA signing
- `ILibDuktape_crypto_verify()` - RSA+SHA verification
- `ILibDuktape_crypto_privateEncrypt()` - RSA private key encryption
- `ILibDuktape_crypto_publicDecrypt()` - RSA public key decryption
- Plus finalizers and internal helpers

**JavaScript API Exposed:**
```javascript
// Hash functions
const hash = require('crypto').createHash('sha256');
hash.update(data);
const digest = hash.digest();

// RSA signing
const sign = require('crypto').createSign('SHA384');
sign.update(data);
const signature = sign.sign(privateKey);
```

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
// <openssl/md5.h>
// <openssl/rsa.h>
```

**Lines Changed:** ~200 insertions, ~120 deletions

**Warnings Eliminated:** 38 deprecation warnings (largest single-file reduction)

---

### 6. microscript/ILibDuktape_Polyfills.c

**Purpose:** JavaScript polyfills and auto-generated compressed modules

**Changes:**
- Migrated SHA-256 hash function for data generator
- Updated module timestamp generation (auto-generated)

**Functions Modified:**
- `ILibDuktape_Polyfills_DataGenerator_Update()` - SHA-256 data hashing

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
```

**Lines Changed:** ~30 insertions, ~20 deletions (plus timestamp updates)

**Warnings Eliminated:** 1 deprecation warning

**Note:** This file contains auto-generated compressed JavaScript modules. Timestamps were updated in a separate commit (4bb68e8).

---

### 7. meshcore/agentcore.c

**Purpose:** MeshAgent core authentication and communication protocol

**Changes:**
- Migrated RSA+SHA-384 signing for agent authentication
- Migrated RSA+SHA-384 verification for server message validation

**Functions Modified:**
- `MeshServer_Sign()` - Sign authentication challenge
- `MeshServer_VerifySignature()` - Verify server signatures

**Authentication Protocol:**
The MeshAgent uses RSA+SHA-384 signatures for:
1. Agent → Server authentication (proving agent identity)
2. Server → Agent message verification (validating commands)

This is **security-critical** code protecting the agent control channel.

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/rsa.h>
// <openssl/sha.h>
```

**Lines Changed:** ~50 insertions, ~35 deletions

**Warnings Eliminated:** 2 deprecation warnings

---

### 8. meshcore/signcheck.c

**Purpose:** Executable signature verification (agent self-update validation)

**Changes:**
- Migrated SHA-384 hash function for file signature verification
- Updated signature block validation for agent updates

**Functions Modified:**
- `signcheck_verifysign()` - Verify executable signatures (POSIX/macOS/Linux path)

**Security Critical:**
This code verifies the cryptographic signature of agent update executables:
1. Reads signature block from end of executable
2. Verifies RSA signature
3. Computes SHA-384 hash of entire executable
4. Compares against signed hash
5. Validates against trusted certificate list

**Any bug here could allow malicious agent updates.**

**Header Changes:**
```c
// Added:
#include <openssl/evp.h>

// Removed direct usage of:
// <openssl/sha.h>
```

**Lines Changed:** ~15 insertions, ~10 deletions

**Warnings Eliminated:** 1 deprecation warning

**Note:** Windows code path uses WinVerifyTrust API, not affected by this migration.

---

## Files NOT Modified

### Intentionally Excluded

**microstack/ILibWebRTC.c**
- Contains 15 HMAC deprecation warnings
- Requires separate EVP_MAC API migration
- Marked as optional/future work
- See [FUTURE_WORK.md](FUTURE_WORK.md) for details

**Other Files with Warnings**
- zlib library functions (deprecated C function syntax, not OpenSSL)
- Duktape JavaScript engine (internal warnings, not OpenSSL)
- macOS-specific warnings (vfork deprecation, not OpenSSL)

---

## Commit History

### Commit: 6295335
**Message:** Migrate cryptographic code from OpenSSL 1.1.1 to OpenSSL 3.x APIs

**Files Changed:** 8 files
- microstack/ILibCrypto.c
- microstack/ILibWebServer.c
- microstack/ILibSimpleDataStore.c
- microscript/ILibDuktape_HttpStream.c
- microscript/ILibDuktape_SHA256.c
- microscript/ILibDuktape_Polyfills.c
- meshcore/agentcore.c
- meshcore/signcheck.c

**Stats:** 542 insertions(+), 329 deletions(-)

---

### Commit: 4bb68e8
**Message:** Update auto-generated module timestamps

**Files Changed:** 1 file
- microscript/ILibDuktape_Polyfills.c

**Stats:** 53 insertions(+), 53 deletions(-)

**Note:** Auto-generated JavaScript module timestamps updated after OpenSSL migration.

---

### Commit: 4349d92
**Message:** Remove unused RSA variable from OpenSSL migration cleanup

**Files Changed:** 1 file
- microstack/ILibCrypto.c

**Stats:** 0 insertions(+), 1 deletion(-)

**Note:** Cleanup of unused `RSA *rsa` variable left over from migration.

---

## Testing Impact

### Files Requiring Functional Testing

**High Priority:**
1. **meshcore/agentcore.c** - Agent authentication (security critical)
2. **meshcore/signcheck.c** - Update signature verification (security critical)
3. **microscript/ILibDuktape_SHA256.c** - JavaScript crypto API (extensive surface area)

**Medium Priority:**
4. **microstack/ILibCrypto.c** - Core crypto utilities (widely used)
5. **microstack/ILibWebServer.c** - HTTP Digest Auth (authentication)
6. **microscript/ILibDuktape_HttpStream.c** - WebSocket handshake

**Low Priority:**
7. **microstack/ILibSimpleDataStore.c** - Data integrity (isolated)
8. **microscript/ILibDuktape_Polyfills.c** - Data generator (rarely used)

### Recommended Test Cases

1. **Agent Authentication:** Full agent → server connection and authentication
2. **Agent Update:** Signature verification of signed executable
3. **JavaScript Crypto:** All hash and signing operations from JavaScript
4. **HTTP Digest Auth:** Web server authentication flow
5. **WebSocket:** WebSocket upgrade handshake
6. **Data Store:** Read/write with integrity checks

---

## Diff Statistics by Category

### By Change Type
| Category | Insertions | Deletions | Net Change |
|----------|------------|-----------|------------|
| Hash Functions (EVP_Digest) | ~350 | ~220 | +130 |
| RSA Operations (EVP_PKEY) | ~150 | ~90 | +60 |
| Error Handling | ~30 | ~10 | +20 |
| Includes/Headers | ~10 | ~10 | 0 |

### By Subsystem
| Subsystem | Files | Insertions | Deletions |
|-----------|-------|------------|-----------|
| JavaScript Bindings | 3 | ~255 | ~155 |
| Core Crypto | 2 | ~200 | ~115 |
| Agent Core | 2 | ~65 | ~45 |
| Web Server | 1 | ~40 | ~25 |

---

*Last updated: November 24, 2025*
