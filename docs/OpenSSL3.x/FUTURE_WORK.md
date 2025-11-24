# Future Work: Optional HMAC Migration

This document describes optional work that was **not included** in the main OpenSSL 3.x migration.

## Status

**Current:** ⚠️ **DEFERRED** - Using deprecated but functional HMAC API
**Priority:** Low - Not critical for OpenSSL 3.x compatibility

---

## Overview: HMAC Warnings

### What Remains

**File:** `microstack/ILibWebRTC.c`
**Warnings:** 15 HMAC deprecation warnings at 3 locations
**Function:** HMAC-SHA1 for STUN/TURN protocol authentication

### Locations

1. **Lines 1430-1434:** `ILibStun_AddMessageIntegrity()` - STUN message authentication
2. **Lines 2058-2062:** `ILibStun_VerifyMessageIntegrityAndErrorCode()` - STUN verification
3. **Lines 6788-6792:** `ILibTURN_ComputeIntegrity()` - TURN authentication

Each location uses the same pattern:
```c
HMAC_CTX *hmac = HMAC_CTX_new();
HMAC_Init_ex(hmac, key, keylen, EVP_sha1(), NULL);
HMAC_Update(hmac, data, datalen);
HMAC_Final(hmac, output, &outlen);
HMAC_CTX_free(hmac);
```

---

## Why This Was NOT Migrated

### Reason 1: Separate Subsystem
- **Different file** from core crypto migration (ILibWebRTC.c vs ILibCrypto.c)
- **Different purpose** - WebRTC protocol vs general cryptography
- **Different API** - HMAC requires EVP_MAC, not EVP_Digest

### Reason 2: Testing Complexity
Requires live WebRTC testing:
- STUN server communication
- TURN relay authentication
- Peer-to-peer WebRTC connections
- Cannot verify with simple unit tests

### Reason 3: Security Critical
HMAC is used for **authentication** in network protocols:
- Prevents packet tampering
- Controls relay server access
- Secures peer connections

**Any bug = broken authentication = security vulnerability**

### Reason 4: Still Functional
The deprecated HMAC API:
- ✅ Still works perfectly in OpenSSL 3.x
- ✅ No functional changes needed
- ✅ Only triggers compiler warnings
- ✅ Will likely remain in OpenSSL 4.x (low removal risk)

---

## Risk Assessment

### Risks of NOT Migrating

| Risk | Severity | Likelihood | Impact |
|------|----------|------------|--------|
| Build warnings remain | Low | 100% | Cosmetic only |
| Future OpenSSL incompatibility | Low | <20% | Would require migration then |
| Missing optimizations | Very Low | Unknown | Unlikely significant |

### Risks of Migrating

| Risk | Severity | Likelihood | Impact |
|------|----------|------------|--------|
| Breaking WebRTC authentication | **Critical** | 5-10% | Service disruption |
| Subtle protocol bugs | **High** | 10-15% | Intermittent failures |
| Testing gaps | High | 50%+ | Undetected issues |
| Development time | Medium | 100% | 4-8 hours |

---

## What HMAC Migration Would Involve

### Current API (OpenSSL 1.1.1 - Deprecated)

```c
#include <openssl/hmac.h>

unsigned char output[20];
unsigned int outlen = 20;

HMAC_CTX *hmac = HMAC_CTX_new();
HMAC_Init_ex(hmac, key, keylen, EVP_sha1(), NULL);
HMAC_Update(hmac, data, datalen);
HMAC_Final(hmac, output, &outlen);
HMAC_CTX_free(hmac);
```

### New API (OpenSSL 3.x - EVP_MAC)

```c
#include <openssl/evp.h>
#include <openssl/params.h>

unsigned char output[20];
size_t outlen = sizeof(output);

// Fetch MAC algorithm
EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
if (mac == NULL) {
    // Handle error
    return -1;
}

// Create context
EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
if (ctx == NULL) {
    EVP_MAC_free(mac);
    return -1;
}

// Set digest algorithm via parameters
OSSL_PARAM params[] = {
    OSSL_PARAM_construct_utf8_string("digest", "SHA1", 0),
    OSSL_PARAM_construct_end()
};

// Initialize with key
if (EVP_MAC_init(ctx, key, keylen, params) <= 0) {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return -1;
}

// Update with data
if (EVP_MAC_update(ctx, data, datalen) <= 0) {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return -1;
}

// Finalize
if (EVP_MAC_final(ctx, output, &outlen, sizeof(output)) <= 0) {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return -1;
}

// Cleanup
EVP_MAC_CTX_free(ctx);
EVP_MAC_free(mac);
```

### Key Differences

1. **More complex API:** 7 steps vs 5 steps
2. **More failure points:** 5 error checks vs 1-2
3. **Parameter structures:** Must use OSSL_PARAM for configuration
4. **Separate MAC object:** Must fetch and free EVP_MAC algorithm
5. **Different includes:** `<openssl/params.h>` required

---

## Testing Requirements

If this migration is pursued, the following tests are **mandatory**:

### 1. STUN Protocol Testing
- [ ] STUN Binding Request with MESSAGE-INTEGRITY attribute
- [ ] STUN response verification
- [ ] Long-term credential authentication
- [ ] Short-term credential authentication
- [ ] Test with real STUN servers (stun.l.google.com, etc.)

### 2. TURN Protocol Testing
- [ ] TURN allocation with authentication
- [ ] Channel binding with integrity
- [ ] Permission installation
- [ ] Data relay with integrity verification
- [ ] Test with real TURN servers

### 3. WebRTC Integration Testing
- [ ] Full peer-to-peer connection establishment
- [ ] ICE candidate gathering through STUN
- [ ] Media relay through TURN
- [ ] Connection through NAT/firewall
- [ ] Multiple simultaneous connections

### 4. Regression Testing
- [ ] Existing agent connections work
- [ ] Remote desktop functionality intact
- [ ] File transfers work
- [ ] Terminal sessions work
- [ ] No performance degradation

---

## Recommended Approach (If Pursued)

### Phase 1: Preparation
1. Set up WebRTC test environment
2. Create test STUN/TURN server
3. Document current behavior
4. Create test cases with known-good results

### Phase 2: Implementation
1. Create helper function for HMAC-SHA1
   ```c
   int util_hmac_sha1(const unsigned char *key, int keylen,
                      const unsigned char *data, int datalen,
                      unsigned char *output, size_t *outlen);
   ```
2. Implement using EVP_MAC API
3. Replace all 3 call sites
4. Verify compilation

### Phase 3: Testing
1. Unit test HMAC function with known test vectors
2. Test STUN message integrity
3. Test TURN authentication
4. Full WebRTC integration test
5. Regression test all agent functionality

### Phase 4: Validation
1. Deploy to test environment
2. Monitor for authentication failures
3. Verify WebRTC connections work
4. Performance comparison
5. Production deployment

**Estimated effort:** 4-8 hours development + 4-8 hours testing

---

## Alternative: Wait for OpenSSL 4.x

### Rationale
- OpenSSL 3.x will be supported until 2026+
- OpenSSL 4.x timeline is unclear (2026-2028?)
- Deprecated APIs often maintained for backward compatibility
- Risk of removal is low in next 3-5 years

### When to Revisit
- OpenSSL 4.0 release announcement
- Confirmation of HMAC API removal
- Major refactoring of ILibWebRTC.c
- Comprehensive WebRTC test suite available

---

## Comparison to Completed Migration

### Digest/RSA Migration (COMPLETED ✅)

| Aspect | Evaluation |
|--------|------------|
| **Files affected** | 8 files, widely distributed |
| **Functions migrated** | 30+ functions |
| **Testing complexity** | Low - can verify with simple unit tests |
| **Security impact** | Medium - used for hashing and signatures |
| **API complexity** | Medium - EVP_Digest* is straightforward |
| **Warnings eliminated** | 65+ warnings |
| **Effort** | ~8-12 hours |

### HMAC Migration (DEFERRED ⚠️)

| Aspect | Evaluation |
|--------|------------|
| **Files affected** | 1 file, isolated |
| **Functions migrated** | 3 locations, same pattern |
| **Testing complexity** | **HIGH - requires live WebRTC testing** |
| **Security impact** | **CRITICAL - breaks authentication if wrong** |
| **API complexity** | **HIGH - EVP_MAC is complex** |
| **Warnings eliminated** | 15 warnings |
| **Effort** | 4-8 hours + extensive testing |

**Conclusion:** HMAC migration has **higher risk** for **lower benefit**.

---

## Decision: Deferred

**Recommendation:** Do NOT migrate HMAC at this time.

**Rationale:**
1. Old API still works perfectly
2. Testing burden is very high
3. Security risk is significant
4. Minimal benefit (warnings only)
5. Can be done later if truly needed

**When to reconsider:**
- OpenSSL 4.x removes HMAC API
- Major WebRTC refactoring occurs
- Comprehensive test suite available
- Security audit requires it

---

## Additional Notes

### Other Warnings in ILibWebRTC.c

The build shows "19 warnings generated" for ILibWebRTC.c, but only 15 are HMAC-related. The other 4 are:

```
microstack/ILibWebRTC.c:3285:6: warning: variable 'rptr' set but not used
microstack/ILibWebRTC.c:4674:9: warning: variable 'tmpval1' set but not used
microstack/ILibWebRTC.c:4674:18: warning: variable 'tmpval2' set but not used
microstack/ILibWebRTC.c:4674:27: warning: variable 'tmpval3' set but not used
```

These are **unused variable warnings**, not OpenSSL deprecation warnings. They could be cleaned up separately if desired.

---

## References

- [OpenSSL EVP_MAC Documentation](https://www.openssl.org/docs/man3.0/man7/EVP_MAC.html)
- [HMAC Migration Example](https://www.openssl.org/docs/man3.0/man7/migration_guide.html#Deprecated-low-level-MAC-functions)
- [STUN RFC 5389](https://tools.ietf.org/html/rfc5389) - MESSAGE-INTEGRITY mechanism
- [TURN RFC 5766](https://tools.ietf.org/html/rfc5766) - Authentication mechanisms

---

*Last updated: November 24, 2025*
