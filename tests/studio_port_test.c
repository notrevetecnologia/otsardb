/* studio_port_test.c — Studio v0.5 port-resolution unit test.
 *
 * Drives otsardb_studio_resolve_port() (src/cli/studio.c) directly — a pure
 * function encoding the `otsardb studio [--port <port>]` precedence:
 * --port flag wins, then OTSARDB_STUDIO_PORT, then the default 8717.
 * -1 = the value actually provided (flag or env) is invalid.
 *
 * The flag PARSING itself (strtol in main.c, usage errors) is CLI behavior
 * exercised manually + documented in ; the precedence and
 * validation rules live in the pure function and are pinned here.
 */

#include "studio.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("studio-port-0093 %-46s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

int main(void) {
    /* default (unchanged from 0070: 8717) */
    check("no flag, no env -> 8717", otsardb_studio_resolve_port(0, NULL) == 8717,
          NULL);
    check("no flag, empty env -> 8717", otsardb_studio_resolve_port(0, "") == 8717,
          NULL);

    /* OTSARDB_STUDIO_PORT fallback */
    check("env 9000 -> 9000", otsardb_studio_resolve_port(0, "9000") == 9000,
          NULL);
    check("env 1 lower bound", otsardb_studio_resolve_port(0, "1") == 1, NULL);
    check("env 65535 upper bound", otsardb_studio_resolve_port(0, "65535") == 65535,
          NULL);

    /* --port flag wins over the env (the v0.5 deliverable) */
    check("flag 9000 wins over env 1234",
          otsardb_studio_resolve_port(9000, "1234") == 9000, NULL);
    check("flag wins over invalid env",
          otsardb_studio_resolve_port(9001, "abc") == 9001, NULL);

    /* invalid env -> -1 (caller reports exit 2) */
    check("env 0 -> -1", otsardb_studio_resolve_port(0, "0") == -1, NULL);
    check("env 65536 -> -1", otsardb_studio_resolve_port(0, "65536") == -1, NULL);
    check("env -1 -> -1", otsardb_studio_resolve_port(0, "-1") == -1, NULL);
    check("env abc -> -1", otsardb_studio_resolve_port(0, "abc") == -1, NULL);
    check("env 90 00 -> -1", otsardb_studio_resolve_port(0, "90 00") == -1, NULL);
    check("env 9000x -> -1", otsardb_studio_resolve_port(0, "9000x") == -1, NULL);
    check("env 0x10 -> -1", otsardb_studio_resolve_port(0, "0x10") == -1, NULL);

    /* invalid flag -> -1 (main.c already rejects these at parse time) */
    check("flag -5 -> -1", otsardb_studio_resolve_port(-5, NULL) == -1, NULL);
    check("flag 65536 -> -1", otsardb_studio_resolve_port(65536, NULL) == -1,
          NULL);

    if (failures) {
        printf("studio-port-0093 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio port resolution (flag > env > 8717)\n");
    return 0;
}
