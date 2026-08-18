#ifndef OTSARDB_CLI_STUDIO_WINDOW_H
#define OTSARDB_CLI_STUDIO_WINDOW_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OtsarDB Studio desktop window (Studio v0.6): a native
 * Win32 window hosting a WebView2 (Edge Chromium) control pointed at the
 * loopback studio server http://127.0.0.1:<port>/, so the workbench feels
 * like a desktop product instead of a browser tab.
 *
 * The WebView2 SDK loader DLL (WebView2Loader.dll) is located AT RUNTIME
 * with LoadLibrary (never a link-time dependency): the binary builds and
 * runs on machines without WebView2 or without the SDK loader. Search
 * order (all documented in the window-mode docs): the evergreen WebView2 Runtime
 * directories under Program Files (x86)/Program Files and %LOCALAPPDATA%
 * (C:\Program Files (x86)\Microsoft\EdgeWebView\Application\<version>\
 * WebView2Loader.dll and friends), the directory of the running
 * executable, the vcpkg store (%VCPKG_ROOT% or C:\vcpkg), and finally the
 * plain system DLL search. The WebView2 COM interfaces used here are
 * declared locally (transcribed from the official WebView2 SDK header,
 * version 1.0.2903.40); only the methods actually called are invoked.
 *
 * When the loader is absent the caller MUST fall back to the v0.5
 * default-browser behavior (browser open + the printed note).
 *
 * Return codes of otsardb_studio_window_open:
 * 0 - the window opened and its message loop ended because the user
 * closed the window: the caller stops the studio server and exits
 * cleanly (WM_CLOSE -> controller Close() -> server stop).
 * 1 - WebView2Loader.dll was not found (error filled with the exact
 * message): the caller prints it and falls back to the browser.
 * 2 - the loader was found but the WebView2 environment or controller
 * could not be created (error filled): the caller falls back to the
 * browser.
 *
 * Windows only. On POSIX builds this file compiles a stub that always
 * returns 1 (the caller's browser fallback), so the studio server keeps
 * working unchanged on non-Windows platforms.
 */
int otsardb_studio_window_open(int port, char *error, size_t error_cap);

#ifdef __cplusplus
}
#endif

#endif
