/*
 * mac_kvm_auth.c
 *
 * Code signature verification for KVM socket connections
 * Ensures only the same meshagent binary can connect
 */

#ifdef __APPLE__

#include "mac_kvm_auth.h"
#include "../../../microstack/ILibParsers.h"
#include <stdio.h>
#include <string.h>

/**
 * Get our own code signature for comparison
 */
SecCodeRef get_self_code(void) {
    SecCodeRef self_code = NULL;
    OSStatus status;

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_GET_SELF_CODE: Getting self code signature...");

    // Get reference to our own running process
    status = SecCodeCopySelf(kSecCSDefaultFlags, &self_code);

    if (status != errSecSuccess) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_GET_SELF_CODE_FAIL: SecCodeCopySelf failed, status=%d", status);
        fprintf(stderr, "KVM Auth: Failed to get self code signature: %d\n", status);
        return NULL;
    }

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_GET_SELF_CODE: Self code signature obtained successfully");
    return self_code;
}

/**
 * Check if two code signatures match (same binary)
 */
int codesign_matches(SecCodeRef code1, SecCodeRef code2) {
    OSStatus status;
    CFDictionaryRef info1 = NULL, info2 = NULL;
    CFDataRef cdhash1 = NULL, cdhash2 = NULL;
    int result = 0;

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH: Comparing code signatures...");

    if (!code1 || !code2) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_FAIL: NULL code reference (code1=%p, code2=%p)", code1, code2);
        return 0;
    }

    // Get signing information from both codes
    status = SecCodeCopySigningInformation(code1, kSecCSSigningInformation, &info1);
    if (status != errSecSuccess) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_FAIL: SecCodeCopySigningInformation(code1) failed, status=%d", status);
        goto cleanup;
    }

    status = SecCodeCopySigningInformation(code2, kSecCSSigningInformation, &info2);
    if (status != errSecSuccess) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_FAIL: SecCodeCopySigningInformation(code2) failed, status=%d", status);
        goto cleanup;
    }

    // Get code directory hashes (unique identifier for the binary)
    cdhash1 = (CFDataRef)CFDictionaryGetValue(info1, kSecCodeInfoUnique);
    cdhash2 = (CFDataRef)CFDictionaryGetValue(info2, kSecCodeInfoUnique);

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH: cdhash1=%p, cdhash2=%p", cdhash1, cdhash2);

    if (cdhash1 && cdhash2) {
        // Compare the unique code directory hashes
        if (CFEqual(cdhash1, cdhash2)) {
            ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_SUCCESS: Code signatures MATCH!");
            result = 1;  // Match!
        } else {
            ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_MISMATCH: Code signatures DO NOT MATCH");
        }
    } else {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH_FAIL: One or both cdhash is NULL");
    }

cleanup:
    if (info1) CFRelease(info1);
    if (info2) CFRelease(info2);

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_CODESIGN_MATCH: Returning result=%d", result);
    return result;
}

/**
 * Verify peer process connected to socket is legitimate meshagent
 */
int verify_peer_codesign(int socket_fd) {
    pid_t peer_pid = 0;
    socklen_t len = sizeof(peer_pid);
    OSStatus status;
    SecCodeRef self_code = NULL;
    SecCodeRef peer_code = NULL;
    int result = 0;

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Starting peer verification for socket_fd=%d", socket_fd);

    // Get PID of connecting process
    if (getsockopt(socket_fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &len) < 0) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: getsockopt(LOCAL_PEERPID) failed: %s", strerror(errno));
        fprintf(stderr, "KVM Auth: Failed to get peer PID: %s\n", strerror(errno));
        return 0;
    }

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Got peer PID=%d", peer_pid);

    if (peer_pid <= 0) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: Invalid peer PID=%d", peer_pid);
        fprintf(stderr, "KVM Auth: Invalid peer PID: %d\n", peer_pid);
        return 0;
    }

    // Get our own code signature
    self_code = get_self_code();
    if (!self_code) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: Failed to get self code signature");
        return 0;
    }

    // Get peer process code signature
    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Getting peer code signature for PID=%d", peer_pid);
    status = SecCodeCreateWithPID(peer_pid, kSecCSDefaultFlags, &peer_code);
    if (status != errSecSuccess) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: SecCodeCreateWithPID failed, PID=%d, status=%d", peer_pid, status);
        fprintf(stderr, "KVM Auth: Failed to get peer code signature (PID %d): %d\n",
                peer_pid, status);
        goto cleanup;
    }

    // Verify peer code is valid (signed, not tampered)
    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Checking peer code validity...");
    status = SecCodeCheckValidity(peer_code, kSecCSDefaultFlags, NULL);
    if (status != errSecSuccess) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: SecCodeCheckValidity failed, PID=%d, status=%d", peer_pid, status);
        fprintf(stderr, "KVM Auth: Peer code signature invalid (PID %d): %d\n",
                peer_pid, status);
        goto cleanup;
    }

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Peer code is valid, comparing signatures...");

    // Compare code signatures - must be same binary
    if (codesign_matches(self_code, peer_code)) {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_SUCCESS: Peer verified successfully, PID=%d", peer_pid);
        result = 1;
    } else {
        ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER_FAIL: Code signature mismatch, PID=%d", peer_pid);
        fprintf(stderr, "KVM Auth: Peer code signature mismatch (PID %d)\n", peer_pid);
    }

cleanup:
    if (self_code) CFRelease(self_code);
    if (peer_code) CFRelease(peer_code);

    ILIBLOGMESSAGEX("MSG_KVM_AUTH_VERIFY_PEER: Returning result=%d", result);
    return result;
}

/**
 * Alternative: Verify using audit token (more secure, avoids PID reuse)
 * Requires macOS 10.14+
 */
#if 0  // Enable if needed
int verify_peer_codesign_audit(int socket_fd) {
    struct xucred cred;
    socklen_t len = sizeof(cred);
    audit_token_t audit_token;
    OSStatus status;
    SecCodeRef self_code = NULL;
    SecCodeRef peer_code = NULL;
    int result = 0;

    // Get peer credentials including audit token
    if (getsockopt(socket_fd, 0, LOCAL_PEERCRED, &cred, &len) < 0) {
        return 0;
    }

    // Note: Getting audit_token requires different approach
    // This is placeholder - actual implementation needs LOCAL_PEERTOKEN (iOS)
    // or parsing /proc/$PID/audit_token

    // Use audit token instead of PID (prevents PID reuse attacks)
    // status = SecCodeCreateWithAuditToken(&audit_token, kSecCSDefaultFlags, &peer_code);

    // ... rest similar to verify_peer_codesign()

    return result;
}
#endif

#endif /* __APPLE__ */
