/*
OpenFrame: log-only check of the control channel's server certificate. See openframe_tls_check.h.
*/

#ifndef MICROSTACK_NOTLS

// Platform headers first: wincrypt.h must come before the OpenSSL headers (openssl/types.h
// undefines its X509_NAME and friends).
#if defined(WIN32)
#include <windows.h>
#include <wincrypt.h>
#define OPENFRAME_TLS_CHECK_SUPPORTED 1
#elif defined(__APPLE__)
#include <pthread.h>
#include <time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#define OPENFRAME_TLS_CHECK_SUPPORTED 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "openframe_tls_check.h"

#ifdef OPENFRAME_TLS_CHECK_SUPPORTED

#define OPENFRAME_TLS_CHECK_HOST_MAX 256
#define OPENFRAME_TLS_CHECK_MAX_CERTS 10
#define OPENFRAME_TLS_CHECK_REASON_MAX 64
#define OPENFRAME_TLS_CHECK_LINE_MAX 512

typedef enum
{
	OpenFrameTlsCheck_Trusted = 0,
	OpenFrameTlsCheck_NotTrusted = 1,
	OpenFrameTlsCheck_Error = 2
} OpenFrameTlsCheck_Verdict;

typedef struct OpenFrameTlsCheck_Pair
{
	char host[OPENFRAME_TLS_CHECK_HOST_MAX];
	unsigned char leaf[32];		// SHA-256 of the leaf certificate
} OpenFrameTlsCheck_Pair;

typedef struct OpenFrameTlsCheck_Job
{
	OpenFrameTlsCheck_LogSink log;
	char host[OPENFRAME_TLS_CHECK_HOST_MAX];
	char leafHex[17];			// first 8 bytes of the leaf SHA-256, to tell certificates apart in the log
	int count;
	unsigned char *der[OPENFRAME_TLS_CHECK_MAX_CERTS];
	int derLen[OPENFRAME_TLS_CHECK_MAX_CERTS];
} OpenFrameTlsCheck_Job;

// Touched only on the chain thread (OpenFrameTlsCheck_Observe).
static OpenFrameTlsCheck_Pair g_openframeTlsPairs[OPENFRAME_TLS_CHECK_MAX_PAIRS];
static int g_openframeTlsPairCount = 0;
static int g_openframeTlsLimitLogged = 0;

// Set by the chain thread, cleared by the worker when its check is done.
#ifdef WIN32
static volatile LONG g_openframeTlsBusy = 0;
#define OPENFRAME_TLS_BUSY_TRY_SET() (InterlockedCompareExchange(&g_openframeTlsBusy, 1, 0) == 0)
#define OPENFRAME_TLS_BUSY_CLEAR() InterlockedExchange(&g_openframeTlsBusy, 0)
#else
static volatile int g_openframeTlsBusy = 0;
#define OPENFRAME_TLS_BUSY_TRY_SET() __sync_bool_compare_and_swap(&g_openframeTlsBusy, 0, 1)
#define OPENFRAME_TLS_BUSY_CLEAR() __sync_lock_release(&g_openframeTlsBusy)
#endif

static unsigned long long OpenFrameTlsCheck_NowMs(void)
{
#ifdef WIN32
	return (unsigned long long)GetTickCount64();
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
	return ((unsigned long long)ts.tv_sec * 1000ULL) + ((unsigned long long)ts.tv_nsec / 1000000ULL);
#endif
}

static void OpenFrameTlsCheck_FreeJob(OpenFrameTlsCheck_Job *job)
{
	int i;
	for (i = 0; i < job->count; ++i) { free(job->der[i]); }
	free(job);
}

#ifdef WIN32
// Same parameters as the client's schannel crate (tls_stream.rs, validate): the chain the server
// sent as extra store, server-auth usage, cache-only revocation, SSL policy with the host name,
// unknown revocation ignored.
static OpenFrameTlsCheck_Verdict OpenFrameTlsCheck_VerifyOs(OpenFrameTlsCheck_Job *job, char *reason, size_t reasonLen)
{
	OpenFrameTlsCheck_Verdict verdict = OpenFrameTlsCheck_Error;
	HCERTSTORE store = NULL;
	PCCERT_CONTEXT leaf = NULL;
	PCCERT_CHAIN_CONTEXT chainContext = NULL;
	CERT_CHAIN_PARA chainPara;
	CERT_CHAIN_POLICY_PARA policyPara;
	CERT_CHAIN_POLICY_STATUS policyStatus;
	SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslPara;
	LPSTR usages[] = { szOID_PKIX_KP_SERVER_AUTH, szOID_SERVER_GATED_CRYPTO, szOID_SGC_NETSCAPE };
	WCHAR wideHost[OPENFRAME_TLS_CHECK_HOST_MAX];
	int i;

	store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
	if (store == NULL) { snprintf(reason, reasonLen, "CertOpenStore 0x%08lx", GetLastError()); goto done; }

	if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, job->der[0], (DWORD)job->derLen[0], CERT_STORE_ADD_ALWAYS, &leaf))
	{
		snprintf(reason, reasonLen, "leaf certificate not parsed 0x%08lx", GetLastError());
		goto done;
	}
	for (i = 1; i < job->count; ++i)
	{
		// An intermediate Windows cannot parse is left out, as the TLS stack would leave it out.
		CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, job->der[i], (DWORD)job->derLen[i], CERT_STORE_ADD_ALWAYS, NULL);
	}

	memset(&chainPara, 0, sizeof(chainPara));
	chainPara.cbSize = sizeof(chainPara);
	chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
	chainPara.RequestedUsage.Usage.cUsageIdentifier = (DWORD)(sizeof(usages) / sizeof(usages[0]));
	chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

	if (!CertGetCertificateChain(NULL, leaf, NULL, store, &chainPara,
		CERT_CHAIN_CACHE_END_CERT | CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY | CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,
		NULL, &chainContext))
	{
		snprintf(reason, reasonLen, "CertGetCertificateChain 0x%08lx", GetLastError());
		goto done;
	}

	if (MultiByteToWideChar(CP_UTF8, 0, job->host, -1, wideHost, OPENFRAME_TLS_CHECK_HOST_MAX) == 0)
	{
		snprintf(reason, reasonLen, "host name not converted 0x%08lx", GetLastError());
		goto done;
	}

	memset(&sslPara, 0, sizeof(sslPara));
	sslPara.cbSize = sizeof(sslPara);
	sslPara.dwAuthType = AUTHTYPE_SERVER;
	sslPara.pwszServerName = wideHost;

	memset(&policyPara, 0, sizeof(policyPara));
	policyPara.cbSize = sizeof(policyPara);
	policyPara.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
	policyPara.pvExtraPolicyPara = &sslPara;

	memset(&policyStatus, 0, sizeof(policyStatus));
	policyStatus.cbSize = sizeof(policyStatus);

	if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chainContext, &policyPara, &policyStatus))
	{
		snprintf(reason, reasonLen, "CertVerifyCertificateChainPolicy 0x%08lx", GetLastError());
		goto done;
	}

	if (policyStatus.dwError == ERROR_SUCCESS)
	{
		verdict = OpenFrameTlsCheck_Trusted;
	}
	else
	{
		verdict = OpenFrameTlsCheck_NotTrusted;
		snprintf(reason, reasonLen, "0x%08lx", (unsigned long)policyStatus.dwError);
	}

done:
	if (chainContext != NULL) { CertFreeCertificateChain(chainContext); }
	if (leaf != NULL) { CertFreeCertificateContext(leaf); }
	if (store != NULL) { CertCloseStore(store, 0); }
	return verdict;
}
#else
// Same steps as the client's security-framework crate (secure_transport.rs, ClientBuilder
// handshake): the chain the server sent, no extra anchors with the system anchors kept, SSL policy
// for the host, default network fetch, SecTrustEvaluateWithError (SecTrustEvaluate before 10.14).
static OpenFrameTlsCheck_Verdict OpenFrameTlsCheck_VerifyOs(OpenFrameTlsCheck_Job *job, char *reason, size_t reasonLen)
{
	OpenFrameTlsCheck_Verdict verdict = OpenFrameTlsCheck_Error;
	CFMutableArrayRef certs = NULL;
	CFArrayRef noAnchors = NULL;
	CFStringRef host = NULL;
	SecPolicyRef policy = NULL;
	SecTrustRef trust = NULL;
	OSStatus status;
	int i;

	certs = CFArrayCreateMutable(kCFAllocatorDefault, job->count, &kCFTypeArrayCallBacks);
	if (certs == NULL) { snprintf(reason, reasonLen, "out of memory"); goto done; }
	for (i = 0; i < job->count; ++i)
	{
		CFDataRef data = CFDataCreate(kCFAllocatorDefault, job->der[i], job->derLen[i]);
		SecCertificateRef cert = (data != NULL) ? SecCertificateCreateWithData(kCFAllocatorDefault, data) : NULL;
		if (data != NULL) { CFRelease(data); }
		if (cert == NULL)
		{
			if (i == 0) { snprintf(reason, reasonLen, "leaf certificate not parsed"); goto done; }
			continue;
		}
		CFArrayAppendValue(certs, cert);
		CFRelease(cert);
	}

	host = CFStringCreateWithCString(kCFAllocatorDefault, job->host, kCFStringEncodingUTF8);
	if (host == NULL) { snprintf(reason, reasonLen, "host name not converted"); goto done; }
	policy = SecPolicyCreateSSL(true, host);
	if (policy == NULL) { snprintf(reason, reasonLen, "SecPolicyCreateSSL failed"); goto done; }

	status = SecTrustCreateWithCertificates(certs, policy, &trust);
	if (status != errSecSuccess || trust == NULL) { snprintf(reason, reasonLen, "SecTrustCreateWithCertificates %d", (int)status); goto done; }

	noAnchors = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
	if (noAnchors == NULL) { snprintf(reason, reasonLen, "out of memory"); goto done; }
	status = SecTrustSetAnchorCertificates(trust, noAnchors);
	if (status != errSecSuccess) { snprintf(reason, reasonLen, "SecTrustSetAnchorCertificates %d", (int)status); goto done; }
	status = SecTrustSetAnchorCertificatesOnly(trust, false);
	if (status != errSecSuccess) { snprintf(reason, reasonLen, "SecTrustSetAnchorCertificatesOnly %d", (int)status); goto done; }

	if (__builtin_available(macOS 10.14, *))
	{
		CFErrorRef error = NULL;
		if (SecTrustEvaluateWithError(trust, &error))
		{
			verdict = OpenFrameTlsCheck_Trusted;
		}
		else
		{
			// The code only: the error's description quotes the certificate's names.
			verdict = OpenFrameTlsCheck_NotTrusted;
			snprintf(reason, reasonLen, "%ld", (error != NULL) ? (long)CFErrorGetCode(error) : 0L);
		}
		if (error != NULL) { CFRelease(error); }
	}
	else
	{
		SecTrustResultType result = kSecTrustResultInvalid;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		status = SecTrustEvaluate(trust, &result);
#pragma clang diagnostic pop
		if (status != errSecSuccess)
		{
			snprintf(reason, reasonLen, "SecTrustEvaluate %d", (int)status);
		}
		else if (result == kSecTrustResultProceed || result == kSecTrustResultUnspecified)
		{
			verdict = OpenFrameTlsCheck_Trusted;
		}
		else
		{
			verdict = OpenFrameTlsCheck_NotTrusted;
			snprintf(reason, reasonLen, "trust result %d", (int)result);
		}
	}

done:
	if (trust != NULL) { CFRelease(trust); }
	if (noAnchors != NULL) { CFRelease(noAnchors); }
	if (policy != NULL) { CFRelease(policy); }
	if (host != NULL) { CFRelease(host); }
	if (certs != NULL) { CFRelease(certs); }
	return verdict;
}
#endif

static void OpenFrameTlsCheck_Run(OpenFrameTlsCheck_Job *job)
{
	char reason[OPENFRAME_TLS_CHECK_REASON_MAX];
	char line[OPENFRAME_TLS_CHECK_LINE_MAX];
	OpenFrameTlsCheck_Verdict verdict = OpenFrameTlsCheck_Error;
	unsigned long long started = OpenFrameTlsCheck_NowMs();
	unsigned long long elapsed;

	reason[0] = 0;
#if defined(WIN32) && defined(_MSC_VER)
	__try
	{
		verdict = OpenFrameTlsCheck_VerifyOs(job, reason, sizeof(reason));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		verdict = OpenFrameTlsCheck_Error;
		snprintf(reason, sizeof(reason), "exception 0x%08lx", (unsigned long)GetExceptionCode());
	}
#else
	verdict = OpenFrameTlsCheck_VerifyOs(job, reason, sizeof(reason));
#endif
	elapsed = OpenFrameTlsCheck_NowMs() - started;

	snprintf(line, sizeof(line), "OpenFrame TLS check: %s (host=%s, leaf=%s, chain=%d%s%s, %llu ms)",
		verdict == OpenFrameTlsCheck_Trusted ? "server certificate trusted by the OS"
		: verdict == OpenFrameTlsCheck_NotTrusted ? "server certificate NOT trusted by the OS, an enforcing agent would refuse this server"
		: "could not evaluate the server certificate",
		job->host, job->leafHex, job->count, reason[0] != 0 ? ", reason=" : "", reason, elapsed);
	job->log(line);
	OpenFrameTlsCheck_FreeJob(job);
	OPENFRAME_TLS_BUSY_CLEAR();
}

#ifdef WIN32
static DWORD WINAPI OpenFrameTlsCheck_Thread(LPVOID arg)
{
	OpenFrameTlsCheck_Run((OpenFrameTlsCheck_Job*)arg);
	return 0;
}
static int OpenFrameTlsCheck_StartWorker(OpenFrameTlsCheck_Job *job)
{
	HANDLE thread = CreateThread(NULL, 0, OpenFrameTlsCheck_Thread, job, 0, NULL);
	if (thread == NULL) { return 0; }
	CloseHandle(thread);
	return 1;
}
#else
static void* OpenFrameTlsCheck_Thread(void *arg)
{
	OpenFrameTlsCheck_Run((OpenFrameTlsCheck_Job*)arg);
	return NULL;
}
static int OpenFrameTlsCheck_StartWorker(OpenFrameTlsCheck_Job *job)
{
	pthread_t thread;
	pthread_attr_t attr;
	int started;
	if (pthread_attr_init(&attr) != 0) { return 0; }
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	started = (pthread_create(&thread, &attr, OpenFrameTlsCheck_Thread, job) == 0);
	pthread_attr_destroy(&attr);
	return started;
}
#endif

void OpenFrameTlsCheck_Observe(STACK_OF(X509) *chain, const char *host, OpenFrameTlsCheck_LogSink log)
{
	OpenFrameTlsCheck_Job *job;
	X509 *leaf;
	unsigned char fingerprint[32];
	unsigned int fingerprintLen = 0;
	size_t hostLen;
	int certCount, i;

	if (log == NULL || host == NULL) { return; }
	hostLen = strlen(host);
	if (hostLen == 0 || hostLen >= OPENFRAME_TLS_CHECK_HOST_MAX) { return; }

	if (chain == NULL || sk_X509_num(chain) < 1) { return; }
	leaf = sk_X509_value(chain, 0);
	if (leaf == NULL) { return; }
	if (X509_digest(leaf, EVP_sha256(), fingerprint, &fingerprintLen) != 1 || fingerprintLen != sizeof(fingerprint)) { return; }

	for (i = 0; i < g_openframeTlsPairCount; ++i)
	{
		if (strcmp(g_openframeTlsPairs[i].host, host) == 0 && memcmp(g_openframeTlsPairs[i].leaf, fingerprint, sizeof(fingerprint)) == 0) { return; }
	}
	if (g_openframeTlsPairCount >= OPENFRAME_TLS_CHECK_MAX_PAIRS)
	{
		if (g_openframeTlsLimitLogged == 0)
		{
			char line[160];
			g_openframeTlsLimitLogged = 1;
			snprintf(line, sizeof(line), "OpenFrame TLS check: %d server certificates checked, further checks skipped until the agent restarts", OPENFRAME_TLS_CHECK_MAX_PAIRS);
			log(line);
		}
		return;
	}

	// One check at a time. A connection made meanwhile is checked on a later reconnect.
	if (!OPENFRAME_TLS_BUSY_TRY_SET()) { return; }

	job = (OpenFrameTlsCheck_Job*)calloc(1, sizeof(OpenFrameTlsCheck_Job));
	if (job == NULL) { OPENFRAME_TLS_BUSY_CLEAR(); return; }
	job->log = log;
	memcpy(job->host, host, hostLen + 1);
	for (i = 0; i < 8; ++i) { snprintf(job->leafHex + (i * 2), 3, "%02x", fingerprint[i]); }

	certCount = sk_X509_num(chain);
	if (certCount > OPENFRAME_TLS_CHECK_MAX_CERTS) { certCount = OPENFRAME_TLS_CHECK_MAX_CERTS; }
	for (i = 0; i < certCount; ++i)
	{
		X509 *cert = sk_X509_value(chain, i);
		unsigned char *out;
		int len = (cert != NULL) ? i2d_X509(cert, NULL) : 0;
		if (len <= 0) { break; }
		if ((job->der[i] = (unsigned char*)malloc((size_t)len)) == NULL) { break; }
		out = job->der[i];
		if (i2d_X509(cert, &out) != len) { free(job->der[i]); job->der[i] = NULL; break; }
		job->derLen[i] = len;
		job->count = i + 1;
	}
	if (job->count == 0) { OpenFrameTlsCheck_FreeJob(job); OPENFRAME_TLS_BUSY_CLEAR(); return; }

	// Recorded before the worker starts, so a pair whose check cannot start is not retried on every reconnect.
	memcpy(g_openframeTlsPairs[g_openframeTlsPairCount].host, host, hostLen + 1);
	memcpy(g_openframeTlsPairs[g_openframeTlsPairCount].leaf, fingerprint, sizeof(fingerprint));
	++g_openframeTlsPairCount;

	if (!OpenFrameTlsCheck_StartWorker(job)) { OpenFrameTlsCheck_FreeJob(job); OPENFRAME_TLS_BUSY_CLEAR(); }
}

#else

void OpenFrameTlsCheck_Observe(STACK_OF(X509) *chain, const char *host, OpenFrameTlsCheck_LogSink log)
{
	(void)chain; (void)host; (void)log;
}

#endif
#endif
