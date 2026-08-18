/* studio_window_test.c — Studio v0.6 window-mode unit test.
 *
 * Drives the pure window-mode decision logic of `otsardb studio --window`
 * without any GUI, sockets or WebView2:
 * - otsardb_studio_window_mode(flag, env): --window flag wins, then
 * OTSARDB_STUDIO_WINDOW (truthy 1/true/yes/on), else 0;
 * - otsardb_studio_window_plan(requested, loader_available): the browser
 * fallback decision (window vs the v0.5 browser path);
 * - otsardb_studio_resolve_port: port resolution smoke checks (the full
 * matrix lives in tests/studio_port_test.c).
 *
 * The GUI itself (src/cli/studio_window.c) is NOT executed here: the
 * window path is a manual test (the loader probe + window open are
 * exercised by `otsardb studio --window` on a real machine).
 */

#include "studio.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("studio-window-0104 %-52s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

int main(void) {
    /* ---- otsardb_studio_window_mode: flag wins, then env truthy ---- */
    check("no flag, no env -> off",
          otsardb_studio_window_mode(0, NULL) == 0, NULL);
    check("no flag, empty env -> off",
          otsardb_studio_window_mode(0, "") == 0, NULL);
    check("flag -> on", otsardb_studio_window_mode(1, NULL) == 1, NULL);
    check("flag wins over env 0",
          otsardb_studio_window_mode(1, "0") == 1, NULL);
    check("flag wins over garbage env",
          otsardb_studio_window_mode(1, "garbage") == 1, NULL);
    check("env 1 -> on", otsardb_studio_window_mode(0, "1") == 1, NULL);
    check("env true -> on", otsardb_studio_window_mode(0, "true") == 1, NULL);
    check("env TRUE -> on", otsardb_studio_window_mode(0, "TRUE") == 1, NULL);
    check("env yes -> on", otsardb_studio_window_mode(0, "yes") == 1, NULL);
    check("env on -> on", otsardb_studio_window_mode(0, "on") == 1, NULL);
    check("env 0 -> off", otsardb_studio_window_mode(0, "0") == 0, NULL);
    check("env false -> off", otsardb_studio_window_mode(0, "false") == 0, NULL);
    check("env off -> off", otsardb_studio_window_mode(0, "off") == 0, NULL);
    check("env 2 -> off (strict truthy)",
          otsardb_studio_window_mode(0, "2") == 0, NULL);
    check("env random text -> off",
          otsardb_studio_window_mode(0, "definitely") == 0, NULL);

    /* ---- otsardb_studio_window_plan: the browser fallback decision ---- */
    check("not requested + loader -> NONE",
          otsardb_studio_window_plan(0, 1) == OTSARDB_STUDIO_WINDOW_PLAN_NONE,
          NULL);
    check("not requested + no loader -> NONE",
          otsardb_studio_window_plan(0, 0) == OTSARDB_STUDIO_WINDOW_PLAN_NONE,
          NULL);
    check("requested + loader -> WINDOW",
          otsardb_studio_window_plan(1, 1) == OTSARDB_STUDIO_WINDOW_PLAN_WINDOW,
          NULL);
    check("requested + no loader -> BROWSER fallback",
          otsardb_studio_window_plan(1, 0) == OTSARDB_STUDIO_WINDOW_PLAN_BROWSER,
          NULL);

    /* ---- otsardb_studio_resolve_port smoke (full matrix: 0093 test) ---- */
    check("port: no flag, no env -> 8717",
          otsardb_studio_resolve_port(0, NULL) == 8717, NULL);
    check("port: env fallback 9000",
          otsardb_studio_resolve_port(0, "9000") == 9000, NULL);
    check("port: flag wins over env",
          otsardb_studio_resolve_port(9100, "9000") == 9100, NULL);
    check("port: invalid env -> -1",
          otsardb_studio_resolve_port(0, "not-a-port") == -1, NULL);

    /* ---- integration shape: mode + plan compose like serve_ex ---- */
    check("compose: no window -> plan NONE",
          otsardb_studio_window_plan(
              otsardb_studio_window_mode(0, NULL), 0) ==
              OTSARDB_STUDIO_WINDOW_PLAN_NONE,
          NULL);
    check("compose: --window on a loader-less machine -> BROWSER fallback",
          otsardb_studio_window_plan(
              otsardb_studio_window_mode(1, "0"), 0) ==
              OTSARDB_STUDIO_WINDOW_PLAN_BROWSER,
          NULL);
    check("compose: OTSARDB_STUDIO_WINDOW=1 + loader -> WINDOW",
          otsardb_studio_window_plan(
              otsardb_studio_window_mode(0, "1"), 1) ==
              OTSARDB_STUDIO_WINDOW_PLAN_WINDOW,
          NULL);

    if (failures) {
        printf("studio-window-0104 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio window-mode decision logic (v0.6)\n");
    return 0;
}
