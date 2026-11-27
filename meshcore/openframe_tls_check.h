/*
OpenFrame: log-only check of the control channel's server certificate (CU-17tkuw698jx, release 1).

The agent still accepts any server certificate. This module only records whether the operating
system's trust store would accept the certificate the server sent, using the same parameters the
OpenFrame client uses for its own HTTPS (native-tls: schannel on Windows, Security.framework on
macOS). The log tells us who a later enforcing release would refuse, before it can refuse anyone.

Bounded so it cannot flood the log:
- each host + leaf certificate pair is checked once per process;
- at most OPENFRAME_TLS_CHECK_MAX_PAIRS pairs per process, then one line and silence;
- one check at a time; a connection that arrives while a check runs is skipped (checked later).

No line contains "Connection FAILED": the client's mesh self-heal restarts the agent on that text.
Other platforms: nothing is checked and nothing is logged.
*/

#ifndef OPENFRAME_TLS_CHECK_H
#define OPENFRAME_TLS_CHECK_H

#ifndef MICROSTACK_NOTLS
#include <openssl/x509.h>

#define OPENFRAME_TLS_CHECK_MAX_PAIRS 8

// Receives one complete log line (no trailing newline), on the chain thread or the worker thread.
typedef void(*OpenFrameTlsCheck_LogSink)(const char *line);

// Call on the chain thread once the control channel's TLS session is up.
// chain: the peer chain as sent, leaf first (SSL_get_peer_cert_chain on the client side); only read
// during the call. host: the name the agent connected to (the SNI). Never blocks, never fails the
// connection: the certificates are copied and verified on a worker thread.
void OpenFrameTlsCheck_Observe(STACK_OF(X509) *chain, const char *host, OpenFrameTlsCheck_LogSink log);

#endif
#endif
