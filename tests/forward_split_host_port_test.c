/* forward_split_host_port_test.c — regression test for the
 * security-audit F09 fix: otsardb_forward_split_host_port
 * must reject CR/LF and whitespace in the host:port input.
 *
 * The client builds the single-line "Host: <host>:<port>" header from the
 * input verbatim (src/ha/forward.c otsardb_forward_client_post), so a \r
 * or \n in either component is an outbound header-injection vector. Before
 * the port component accepted leading whitespace through
 * strtol, so "1.2.3.4:80\r\nX: y" and "1.2.3.4:\r\n80" both passed the
 * split and injected a header into the forwarded request.
 *
 * Pure function test: no sockets, no store — deterministic on every build
 * (core and S3) and every platform.
 */

#include "forward.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_ok(const char *name, const char *host_port,
                      const char *want_host, int want_port) {
    char host[256];
    int port = 0;
    int rc = otsardb_forward_split_host_port(host_port, host, sizeof(host),
                                           &port);
    int ok = rc == 1 && strcmp(host, want_host) == 0 && port == want_port;
    printf("forward-f09 %-46s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("  detail: input \"%s\" rc=%d host=\"%s\" port=%d\n", host_port,
               rc, host, port);
        failures++;
    }
}

static void expect_rejected(const char *name, const char *host_port) {
    char host[256];
    int port = 0;
    int rc = otsardb_forward_split_host_port(host_port, host, sizeof(host),
                                           &port);
    printf("forward-f09 %-46s %s\n", name, rc == 0 ? "PASS" : "FAIL");
    if (rc != 0) {
        printf("  detail: input \"%s\" was accepted (host=\"%s\" port=%d)\n",
               host_port, host, port);
        failures++;
    }
}

int main(void) {
    /* --- still-valid inputs (pre-0147 contract preserved) --- */
    expect_ok("valid 127.0.0.1:18080", "127.0.0.1:18080", "127.0.0.1", 18080);
    expect_ok("valid localhost:80", "localhost:80", "localhost", 80);
    expect_ok("valid host.sub.example:443", "host.sub.example:443",
              "host.sub.example", 443);

    /* --- F09: CR/LF rejected (header injection) --- */
    expect_rejected("CRLF in host (1.2.3.4\\r\\nX: y:80)", "1.2.3.4\r\nX: y:80");
    expect_rejected("CRLF in port (1.2.3.4:80\\r\\nX: y)",
                    "1.2.3.4:80\r\nX: y");
    expect_rejected("CRLF before port digits (1.2.3.4:\\r\\n80)",
                    "1.2.3.4:\r\n80");
    expect_rejected("CR alone in host", "1.2.3.4\r:80");
    expect_rejected("LF alone in host", "1.2.3.4\n:80");

    /* --- F09: whitespace rejected --- */
    expect_rejected("space in host (my host:80)", "my host:80");
    expect_rejected("tab in host", "my\thost:80");
    expect_rejected("trailing space after port", "1.2.3.4:80 ");
    expect_rejected("leading space before host", " 1.2.3.4:80");

    /* --- pre-existing rejection cases (unchanged) --- */
    expect_rejected("no port separator (no-port)", "no-port");
    expect_rejected("empty host (:5)", ":5");
    expect_rejected("port out of range (h:99999)", "h:99999");
    expect_rejected("non-numeric port (h:abc)", "h:abc");
    expect_rejected("empty input", "");
    expect_rejected("port 0 (h:0)", "h:0");

    if (failures) {
        printf("forward-f09 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: forward split_host_port rejects CR/LF and whitespace "
           "(audit F09)\n");
    return 0;
}
