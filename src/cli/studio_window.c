/* studio_window.c — OtsarDB Studio desktop window (v0.6).
 *
 * A native Win32 window hosting a WebView2 (Edge Chromium) control pointed
 * at the loopback studio server http://127.0.0.1:<port>/. The server runs
 * on its own thread (studio.c); this file owns the window thread (the main
 * thread), the message loop and the WebView2 controller. WM_CLOSE on the
 * host window closes the controller; the message loop then ends, the
 * function returns 0 and the caller (otsardb_studio_serve_ex) stops the
 * server and exits cleanly.
 *
 * WebView2 is deliberately NOT a build dependency: WebView2Loader.dll (the
 * SDK loader) is located at runtime via LoadLibrary and the exported
 * CreateCoreWebView2EnvironmentWithOptions is fetched with
 * GetProcAddress. The WebView2 COM interfaces are declared locally,
 * transcribed method-for-method and IID-for-IID from the official
 * WebView2 SDK header (WebView2.h, SDK 1.0.2903.40, package
 * Microsoft.Web.WebView2, BSD-3-Clause): only the subset of methods this
 * window actually calls is invoked; every vtable member up to the last
 * used one is declared so the layout matches the runtime exactly
 * (interfaces are append-only, so truncating after the last used method is
 * layout-safe). No WebView2 SDK header or library is required to build.
 *
 * The desktop-shell model follows the vision's option (b) record
 * (STUDIO_VISION.md §4b — native webview-style "system webview shell",
 * REIMPLEMENTED without a Rust toolchain) and the market-standard WebView2
 * embedding pattern (the canonical Microsoft WebView2 sample flow:
 * environment -> controller -> navigate). Nothing is imported; this is an
 * ORIGINAL minimal shell written against the public COM interfaces.
 *
 * POSIX: this file compiles a stub that always reports the window as
 * unavailable; the caller's browser fallback (the v0.5 path) then runs.
 */

#include "studio_window.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

/* -------------------------------------------------- WebView2 interfaces
 * Transcribed from WebView2.h (SDK 1.0.2903.40). Only the method order and
 * the IIDs matter for the ABI; signatures of the members actually called
 * are exact, the rest are declared for layout fidelity (opaque handler
 * types where the full declaration is not needed). */

typedef struct ICoreWebView2Environment ICoreWebView2Environment;
typedef struct ICoreWebView2Controller ICoreWebView2Controller;
typedef struct ICoreWebView2 ICoreWebView2;
typedef struct ICoreWebView2Settings ICoreWebView2Settings;
typedef struct ICoreWebView2NavigationCompletedEventArgs
    ICoreWebView2NavigationCompletedEventArgs;
typedef struct ICoreWebView2NavigationCompletedEventHandler
    ICoreWebView2NavigationCompletedEventHandler;
typedef struct ICoreWebView2NewWindowRequestedEventArgs
    ICoreWebView2NewWindowRequestedEventArgs;
typedef struct ICoreWebView2NewWindowRequestedEventHandler
    ICoreWebView2NewWindowRequestedEventHandler;
typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
typedef struct ICoreWebView2EnvironmentOptions ICoreWebView2EnvironmentOptions;
typedef struct ICoreWebView2WebResourceResponse ICoreWebView2WebResourceResponse;
typedef struct ICoreWebView2NavigationStartingEventHandler
    ICoreWebView2NavigationStartingEventHandler;
typedef struct ICoreWebView2ContentLoadingEventHandler
    ICoreWebView2ContentLoadingEventHandler;
typedef struct ICoreWebView2SourceChangedEventHandler
    ICoreWebView2SourceChangedEventHandler;
typedef struct ICoreWebView2HistoryChangedEventHandler
    ICoreWebView2HistoryChangedEventHandler;
typedef struct ICoreWebView2ScriptDialogOpeningEventHandler
    ICoreWebView2ScriptDialogOpeningEventHandler;
typedef struct ICoreWebView2PermissionRequestedEventHandler
    ICoreWebView2PermissionRequestedEventHandler;
typedef struct ICoreWebView2ProcessFailedEventHandler
    ICoreWebView2ProcessFailedEventHandler;
typedef struct ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler;
typedef struct ICoreWebView2ExecuteScriptCompletedHandler
    ICoreWebView2ExecuteScriptCompletedHandler;
typedef struct ICoreWebView2CapturePreviewCompletedHandler
    ICoreWebView2CapturePreviewCompletedHandler;
typedef struct ICoreWebView2WebMessageReceivedEventHandler
    ICoreWebView2WebMessageReceivedEventHandler;
typedef struct ICoreWebView2CallDevToolsProtocolMethodCompletedHandler
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;
typedef struct ICoreWebView2DevToolsProtocolEventReceiver
    ICoreWebView2DevToolsProtocolEventReceiver;
typedef struct ICoreWebView2Deferral ICoreWebView2Deferral;
typedef struct ICoreWebView2WindowFeatures ICoreWebView2WindowFeatures;

typedef struct _EventRegistrationToken {
    LONGLONG value;
} EventRegistrationToken;

typedef int COREWEBVIEW2_MOVE_FOCUS_REASON;
typedef int COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT;
typedef int COREWEBVIEW2_WEB_ERROR_STATUS;

/* IIDs — verbatim from WebView2.h 1.0.2903.40. */
static const GUID IID_WV2_ENVIRONMENT = {0xb96d755e, 0x0319, 0x4e92,
    {0xa2, 0x96, 0x23, 0x43, 0x6f, 0x46, 0xa1, 0xfc}};
static const GUID IID_WV2_CONTROLLER = {0x4d00c0d1, 0x9434, 0x4eb6,
    {0x80, 0x78, 0x86, 0x97, 0xa5, 0x60, 0x33, 0x4f}};
static const GUID IID_WV2_CORE = {0x76eceacb, 0x0462, 0x4d94,
    {0xac, 0x83, 0x42, 0x3a, 0x67, 0x93, 0x77, 0x5e}};
static const GUID IID_WV2_ENV_HANDLER = {0x4e8a3389, 0xc9d8, 0x4bd2,
    {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
static const GUID IID_WV2_CTRL_HANDLER = {0x6c4819f3, 0xc9b7, 0x4260,
    {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};
static const GUID IID_WV2_NAV_EVENT = {0xd33a35bf, 0x1c49, 0x4f98,
    {0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c}};
static const GUID IID_WV2_NAV_ARGS = {0x30d68b7d, 0x20d9, 0x4752,
    {0xa9, 0xca, 0xec, 0x84, 0x48, 0xfb, 0xb5, 0xc1}};
static const GUID IID_WV2_NEW_EVENT = {0xd4c185fe, 0xc81c, 0x4989,
    {0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51}};
static const GUID IID_WV2_NEW_ARGS = {0x34acb11c, 0xfc37, 0x4418,
    {0x91, 0x32, 0xf9, 0xc2, 0x1d, 0x1e, 0xaf, 0xb9}};

/* ---- ICoreWebView2Environment (members used: CreateCoreWebView2Controller
 * and get_BrowserVersionString; CreateWebResourceResponse declared for
 * layout). */
typedef struct ICoreWebView2EnvironmentVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Environment *This,
        REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2Environment *This);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2Environment *This);
    HRESULT(STDMETHODCALLTYPE *CreateCoreWebView2Controller)(
        ICoreWebView2Environment *This, HWND parentWindow,
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler);
    HRESULT(STDMETHODCALLTYPE *CreateWebResourceResponse)(
        ICoreWebView2Environment *This, IStream *content, int statusCode,
        LPCWSTR reasonPhrase, LPCWSTR headers,
        ICoreWebView2WebResourceResponse **response);
    HRESULT(STDMETHODCALLTYPE *get_BrowserVersionString)(
        ICoreWebView2Environment *This, LPWSTR *versionInfo);
} ICoreWebView2EnvironmentVtbl;

struct ICoreWebView2Environment {
    ICoreWebView2EnvironmentVtbl *lpVtbl;
};

/* ---- ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler. */
typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
        REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This);
    HRESULT(STDMETHODCALLTYPE *Invoke)(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
        HRESULT errorCode, ICoreWebView2Environment *result);
} ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
};

/* ---- ICoreWebView2CreateCoreWebView2ControllerCompletedHandler. */
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
        REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
    HRESULT(STDMETHODCALLTYPE *Invoke)(
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
        HRESULT errorCode, ICoreWebView2Controller *result);
} ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
};

/* ---- ICoreWebView2Controller (members used: put_IsVisible, put_Bounds,
 * put_ParentWindow, Close, get_CoreWebView2; the rest up to
 * get_CoreWebView2 are declared for layout). */
typedef struct ICoreWebView2ControllerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Controller *This,
        REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2Controller *This);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2Controller *This);
    HRESULT(STDMETHODCALLTYPE *get_IsVisible)(ICoreWebView2Controller *This,
        BOOL *isVisible);
    HRESULT(STDMETHODCALLTYPE *put_IsVisible)(ICoreWebView2Controller *This,
        BOOL isVisible);
    HRESULT(STDMETHODCALLTYPE *get_Bounds)(ICoreWebView2Controller *This,
        RECT *bounds);
    HRESULT(STDMETHODCALLTYPE *put_Bounds)(ICoreWebView2Controller *This,
        RECT bounds);
    HRESULT(STDMETHODCALLTYPE *get_ZoomFactor)(ICoreWebView2Controller *This,
        double *zoomFactor);
    HRESULT(STDMETHODCALLTYPE *put_ZoomFactor)(ICoreWebView2Controller *This,
        double zoomFactor);
    HRESULT(STDMETHODCALLTYPE *add_ZoomFactorChanged)(
        ICoreWebView2Controller *This,
        void *eventHandler, EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_ZoomFactorChanged)(
        ICoreWebView2Controller *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *SetBoundsAndZoomFactor)(
        ICoreWebView2Controller *This, RECT bounds, double zoomFactor);
    HRESULT(STDMETHODCALLTYPE *MoveFocus)(ICoreWebView2Controller *This,
        COREWEBVIEW2_MOVE_FOCUS_REASON reason);
    HRESULT(STDMETHODCALLTYPE *add_MoveFocusRequested)(
        ICoreWebView2Controller *This, void *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_MoveFocusRequested)(
        ICoreWebView2Controller *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_GotFocus)(ICoreWebView2Controller *This,
        void *eventHandler, EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_GotFocus)(
        ICoreWebView2Controller *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_LostFocus)(ICoreWebView2Controller *This,
        void *eventHandler, EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_LostFocus)(
        ICoreWebView2Controller *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_AcceleratorKeyPressed)(
        ICoreWebView2Controller *This, void *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_AcceleratorKeyPressed)(
        ICoreWebView2Controller *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *get_ParentWindow)(
        ICoreWebView2Controller *This, HWND *parentWindow);
    HRESULT(STDMETHODCALLTYPE *put_ParentWindow)(
        ICoreWebView2Controller *This, HWND parentWindow);
    HRESULT(STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(
        ICoreWebView2Controller *This);
    HRESULT(STDMETHODCALLTYPE *Close)(ICoreWebView2Controller *This);
    HRESULT(STDMETHODCALLTYPE *get_CoreWebView2)(
        ICoreWebView2Controller *This, ICoreWebView2 **coreWebView2);
} ICoreWebView2ControllerVtbl;

struct ICoreWebView2Controller {
    ICoreWebView2ControllerVtbl *lpVtbl;
};

/* ---- ICoreWebView2 (members used: Navigate, add_NavigationCompleted,
 * add_NewWindowRequested; every member up to add_NewWindowRequested is
 * declared for layout — the WebView2 interface is append-only, so the
 * earlier members are stable). */
typedef struct ICoreWebView2Vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2 *This,
        REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2 *This);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2 *This);
    HRESULT(STDMETHODCALLTYPE *get_Settings)(ICoreWebView2 *This,
        ICoreWebView2Settings **settings);
    HRESULT(STDMETHODCALLTYPE *get_Source)(ICoreWebView2 *This,
        LPWSTR *uri);
    HRESULT(STDMETHODCALLTYPE *Navigate)(ICoreWebView2 *This, LPCWSTR uri);
    HRESULT(STDMETHODCALLTYPE *NavigateToString)(ICoreWebView2 *This,
        LPCWSTR htmlContent);
    HRESULT(STDMETHODCALLTYPE *add_NavigationStarting)(
        ICoreWebView2 *This, ICoreWebView2NavigationStartingEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_NavigationStarting)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_ContentLoading)(ICoreWebView2 *This,
        ICoreWebView2ContentLoadingEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_ContentLoading)(ICoreWebView2 *This,
        EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_SourceChanged)(ICoreWebView2 *This,
        ICoreWebView2SourceChangedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_SourceChanged)(ICoreWebView2 *This,
        EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_HistoryChanged)(ICoreWebView2 *This,
        ICoreWebView2HistoryChangedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_HistoryChanged)(ICoreWebView2 *This,
        EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_NavigationCompleted)(
        ICoreWebView2 *This, ICoreWebView2NavigationCompletedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_NavigationCompleted)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_FrameNavigationStarting)(
        ICoreWebView2 *This, ICoreWebView2NavigationStartingEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_FrameNavigationStarting)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_FrameNavigationCompleted)(
        ICoreWebView2 *This, ICoreWebView2NavigationCompletedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_FrameNavigationCompleted)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_ScriptDialogOpening)(
        ICoreWebView2 *This, ICoreWebView2ScriptDialogOpeningEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_ScriptDialogOpening)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_PermissionRequested)(
        ICoreWebView2 *This, ICoreWebView2PermissionRequestedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_PermissionRequested)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *add_ProcessFailed)(
        ICoreWebView2 *This, ICoreWebView2ProcessFailedEventHandler *eventHandler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_ProcessFailed)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *AddScriptToExecuteOnDocumentCreated)(
        ICoreWebView2 *This, LPCWSTR javaScript,
        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *handler,
        LPWSTR *id);
    HRESULT(STDMETHODCALLTYPE *RemoveScriptToExecuteOnDocumentCreated)(
        ICoreWebView2 *This, LPCWSTR id);
    HRESULT(STDMETHODCALLTYPE *ExecuteScript)(ICoreWebView2 *This,
        LPCWSTR javaScript, ICoreWebView2ExecuteScriptCompletedHandler *handler);
    HRESULT(STDMETHODCALLTYPE *CapturePreview)(ICoreWebView2 *This,
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT imageFormat, IStream *imageStream,
        ICoreWebView2CapturePreviewCompletedHandler *handler);
    HRESULT(STDMETHODCALLTYPE *Reload)(ICoreWebView2 *This);
    HRESULT(STDMETHODCALLTYPE *PostWebMessageAsJson)(ICoreWebView2 *This,
        LPCWSTR webMessageAsJson);
    HRESULT(STDMETHODCALLTYPE *PostWebMessageAsString)(ICoreWebView2 *This,
        LPCWSTR webMessageAsString);
    HRESULT(STDMETHODCALLTYPE *add_WebMessageReceived)(
        ICoreWebView2 *This, ICoreWebView2WebMessageReceivedEventHandler *handler,
        EventRegistrationToken *token);
    HRESULT(STDMETHODCALLTYPE *remove_WebMessageReceived)(
        ICoreWebView2 *This, EventRegistrationToken token);
    HRESULT(STDMETHODCALLTYPE *CallDevToolsProtocolMethod)(
        ICoreWebView2 *This, LPCWSTR methodName, LPCWSTR parametersAsJson,
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *handler);
    HRESULT(STDMETHODCALLTYPE *get_BrowserProcessId)(ICoreWebView2 *This,
        UINT32 *value);
    HRESULT(STDMETHODCALLTYPE *get_CanGoBack)(ICoreWebView2 *This,
        BOOL *canGoBack);
    HRESULT(STDMETHODCALLTYPE *get_CanGoForward)(ICoreWebView2 *This,
        BOOL *canGoForward);
    HRESULT(STDMETHODCALLTYPE *GoBack)(ICoreWebView2 *This);
    HRESULT(STDMETHODCALLTYPE *GoForward)(ICoreWebView2 *This);
    HRESULT(STDMETHODCALLTYPE *GetDevToolsProtocolEventReceiver)(
        ICoreWebView2 *This, LPCWSTR eventName,
        ICoreWebView2DevToolsProtocolEventReceiver **receiver);
    HRESULT(STDMETHODCALLTYPE *Stop)(ICoreWebView2 *This);
    HRESULT(STDMETHODCALLTYPE *add_NewWindowRequested)(
        ICoreWebView2 *This, ICoreWebView2NewWindowRequestedEventHandler *eventHandler,
        EventRegistrationToken *token);
} ICoreWebView2Vtbl;

struct ICoreWebView2 {
    ICoreWebView2Vtbl *lpVtbl;
};

/* ---- Navigation events. */
typedef struct ICoreWebView2NavigationCompletedEventArgsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2NavigationCompletedEventArgs *This, REFIID riid,
        void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2NavigationCompletedEventArgs *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2NavigationCompletedEventArgs *This);
    HRESULT(STDMETHODCALLTYPE *get_IsSuccess)(
        ICoreWebView2NavigationCompletedEventArgs *This, BOOL *isSuccess);
    HRESULT(STDMETHODCALLTYPE *get_WebErrorStatus)(
        ICoreWebView2NavigationCompletedEventArgs *This,
        COREWEBVIEW2_WEB_ERROR_STATUS *webErrorStatus);
    HRESULT(STDMETHODCALLTYPE *get_NavigationId)(
        ICoreWebView2NavigationCompletedEventArgs *This, UINT64 *navigationId);
} ICoreWebView2NavigationCompletedEventArgsVtbl;

struct ICoreWebView2NavigationCompletedEventArgs {
    ICoreWebView2NavigationCompletedEventArgsVtbl *lpVtbl;
};

typedef struct ICoreWebView2NavigationCompletedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2NavigationCompletedEventHandler *This, REFIID riid,
        void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2NavigationCompletedEventHandler *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2NavigationCompletedEventHandler *This);
    HRESULT(STDMETHODCALLTYPE *Invoke)(
        ICoreWebView2NavigationCompletedEventHandler *This,
        ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args);
} ICoreWebView2NavigationCompletedEventHandlerVtbl;

struct ICoreWebView2NavigationCompletedEventHandler {
    ICoreWebView2NavigationCompletedEventHandlerVtbl *lpVtbl;
};

/* ---- New-window events (put_Handled suppresses popups; single-window
 * workbench). */
typedef struct ICoreWebView2NewWindowRequestedEventArgsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2NewWindowRequestedEventArgs *This, REFIID riid,
        void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2NewWindowRequestedEventArgs *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2NewWindowRequestedEventArgs *This);
    HRESULT(STDMETHODCALLTYPE *get_Uri)(
        ICoreWebView2NewWindowRequestedEventArgs *This, LPWSTR *uri);
    HRESULT(STDMETHODCALLTYPE *put_NewWindow)(
        ICoreWebView2NewWindowRequestedEventArgs *This,
        ICoreWebView2 *newWindow);
    HRESULT(STDMETHODCALLTYPE *get_NewWindow)(
        ICoreWebView2NewWindowRequestedEventArgs *This,
        ICoreWebView2 **newWindow);
    HRESULT(STDMETHODCALLTYPE *put_Handled)(
        ICoreWebView2NewWindowRequestedEventArgs *This, BOOL handled);
    HRESULT(STDMETHODCALLTYPE *get_Handled)(
        ICoreWebView2NewWindowRequestedEventArgs *This, BOOL *handled);
    HRESULT(STDMETHODCALLTYPE *get_IsUserInitiated)(
        ICoreWebView2NewWindowRequestedEventArgs *This,
        BOOL *isUserInitiated);
    HRESULT(STDMETHODCALLTYPE *GetDeferral)(
        ICoreWebView2NewWindowRequestedEventArgs *This,
        ICoreWebView2Deferral **deferral);
    HRESULT(STDMETHODCALLTYPE *get_WindowFeatures)(
        ICoreWebView2NewWindowRequestedEventArgs *This,
        ICoreWebView2WindowFeatures **value);
} ICoreWebView2NewWindowRequestedEventArgsVtbl;

struct ICoreWebView2NewWindowRequestedEventArgs {
    ICoreWebView2NewWindowRequestedEventArgsVtbl *lpVtbl;
};

typedef struct ICoreWebView2NewWindowRequestedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(
        ICoreWebView2NewWindowRequestedEventHandler *This, REFIID riid,
        void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(
        ICoreWebView2NewWindowRequestedEventHandler *This);
    ULONG(STDMETHODCALLTYPE *Release)(
        ICoreWebView2NewWindowRequestedEventHandler *This);
    HRESULT(STDMETHODCALLTYPE *Invoke)(
        ICoreWebView2NewWindowRequestedEventHandler *This,
        ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args);
} ICoreWebView2NewWindowRequestedEventHandlerVtbl;

struct ICoreWebView2NewWindowRequestedEventHandler {
    ICoreWebView2NewWindowRequestedEventHandlerVtbl *lpVtbl;
};

/* The loader export: WebView2Loader.dll -> CreateCoreWebView2EnvironmentWithOptions. */
typedef HRESULT(STDMETHODCALLTYPE *wv2_create_environment_fn)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

/* ------------------------------------------------------------ window ctx */

typedef struct wv_ctx wv_ctx;

typedef struct {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
    long refs;
    wv_ctx *ctx;
} wv_env_handler;

typedef struct {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
    long refs;
    wv_ctx *ctx;
} wv_ctrl_handler;

typedef struct {
    ICoreWebView2NavigationCompletedEventHandlerVtbl *lpVtbl;
    long refs;
    wv_ctx *ctx;
} wv_nav_handler;

typedef struct {
    ICoreWebView2NewWindowRequestedEventHandlerVtbl *lpVtbl;
    long refs;
    wv_ctx *ctx;
} wv_new_handler;

struct wv_ctx {
    int result;            /* 0 = clean window close, 2 = setup failure */
    char error[256];
    wchar_t url[128];
    HWND hwnd;
    HMODULE loader;
    ICoreWebView2Environment *environment;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *webview;
    EventRegistrationToken nav_token;
    EventRegistrationToken new_token;
    wv_env_handler env_handler;
    wv_ctrl_handler ctrl_handler;
    wv_nav_handler nav_handler;
    wv_new_handler new_handler;
};

/* --------------------------------------------------- loader discovery */

static int wv_file_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Searches <base>\<version-dir>\WebView2Loader.dll: enumerates the version
 * subdirectories of the evergreen WebView2 Runtime install. Returns 1 when
 * found (path copied into out). */
static int wv_search_version_dirs(const wchar_t *base, wchar_t *out,
                                  size_t out_cap) {
    wchar_t pattern[1024];
    if (wcslen(base) + 3 >= 1024) return 0;
    _snwprintf_s(pattern, 1024, _TRUNCATE, L"%s\\*", base);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        _snwprintf_s(out, out_cap, _TRUNCATE, L"%s\\%s\\WebView2Loader.dll",
                     base, fd.cFileName);
        if (wv_file_exists(out)) {
            found = 1;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static int wv_probe_path(const wchar_t *path) {
    return path && wv_file_exists(path);
}

/* Runtime loader discovery. Search order (documented in the window-mode docs):
 * 1. evergreen WebView2 Runtime version dirs under Program Files (x86),
 * 2. ... under Program Files, 3. ... under %LOCALAPPDATA%,
 * 4. the directory of the running executable, 5. the vcpkg store
 * (%VCPKG_ROOT% or C:\vcpkg), 6. the plain system DLL search. Fills
 * loader_path (first hit) and returns 1 when a loadable loader exists. */
static int wv_find_loader(wchar_t *loader_path, size_t loader_path_cap) {
    wchar_t env_buf[1024];
    DWORD len;
    wchar_t exe_path[2048];

    loader_path[0] = L'\0';

    /* 1-3: EdgeWebView runtime version dirs. */
    len = GetEnvironmentVariableW(L"ProgramFiles(x86)", env_buf, 1024);
    if (len > 0 && len < 1024) {
        wchar_t base[1088];
        _snwprintf_s(base, 1088, _TRUNCATE, L"%s\\Microsoft\\EdgeWebView\\Application",
                     env_buf);
        if (wv_search_version_dirs(base, loader_path, loader_path_cap)) return 1;
    }
    len = GetEnvironmentVariableW(L"ProgramFiles", env_buf, 1024);
    if (len > 0 && len < 1024) {
        wchar_t base[1088];
        _snwprintf_s(base, 1088, _TRUNCATE, L"%s\\Microsoft\\EdgeWebView\\Application",
                     env_buf);
        if (wv_search_version_dirs(base, loader_path, loader_path_cap)) return 1;
    }
    len = GetEnvironmentVariableW(L"LOCALAPPDATA", env_buf, 1024);
    if (len > 0 && len < 1024) {
        wchar_t base[1088];
        _snwprintf_s(base, 1088, _TRUNCATE,
                     L"%s\\Microsoft\\EdgeWebView\\Application", env_buf);
        if (wv_search_version_dirs(base, loader_path, loader_path_cap)) return 1;
    }

    /* 4: the directory of the running executable. */
    if (GetModuleFileNameW(NULL, exe_path, 2048) > 0) {
        wchar_t *slash = wcsrchr(exe_path, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf_s(loader_path, loader_path_cap, _TRUNCATE,
                         L"%s\\WebView2Loader.dll", exe_path);
            if (wv_probe_path(loader_path)) return 1;
        }
    }

    /* 5: the vcpkg store (%VCPKG_ROOT% or C:\vcpkg): <root>\installed\
     * <triplet>\bin\WebView2Loader.dll for the common triplets, then any
     * <root>\installed\<dir>\bin hit. */
    wchar_t vcpkg_root[1024];
    len = GetEnvironmentVariableW(L"VCPKG_ROOT", vcpkg_root, 1024);
    if (len == 0 || len >= 1024) {
        _snwprintf_s(vcpkg_root, 1024, _TRUNCATE, L"C:\\vcpkg");
    }
    const wchar_t *triplets[] = {L"x64-windows", L"x64-windows-static",
                                 L"x64-windows-static-md", L"arm64-windows",
                                 L"x86-windows"};
    for (size_t i = 0; i < sizeof(triplets) / sizeof(triplets[0]); ++i) {
        _snwprintf_s(loader_path, loader_path_cap, _TRUNCATE,
                     L"%s\\installed\\%s\\bin\\WebView2Loader.dll", vcpkg_root,
                     triplets[i]);
        if (wv_probe_path(loader_path)) return 1;
    }
    {
        wchar_t pattern[1104];
        _snwprintf_s(pattern, 1104, _TRUNCATE, L"%s\\installed\\*", vcpkg_root);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 ||
                    wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                _snwprintf_s(loader_path, loader_path_cap, _TRUNCATE,
                             L"%s\\installed\\%s\\bin\\WebView2Loader.dll",
                             vcpkg_root, fd.cFileName);
                if (wv_probe_path(loader_path)) {
                    FindClose(h);
                    return 1;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    /* 6: the plain system DLL search. */
    HMODULE probe = LoadLibraryW(L"WebView2Loader.dll");
    if (probe) {
        FreeLibrary(probe);
        _snwprintf_s(loader_path, loader_path_cap, _TRUNCATE,
                     L"WebView2Loader.dll");
        return 1;
    }

    loader_path[0] = L'\0';
    return 0;
}

/* ------------------------------------------------------- COM callbacks */

static HRESULT STDMETHODCALLTYPE wv_env_handler_qi(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    REFIID riid, void **ppvObject) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_WV2_ENV_HANDLER)) {
        wv_env_handler *h = (wv_env_handler *)This;
        ++h->refs;
        *ppvObject = This;
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE wv_env_handler_addref(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    wv_env_handler *h = (wv_env_handler *)This;
    return (ULONG)++h->refs;
}

static ULONG STDMETHODCALLTYPE wv_env_handler_release(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    wv_env_handler *h = (wv_env_handler *)This;
    long refs = --h->refs;
    /* Embedded object: no free (owned by wv_ctx). */
    return (ULONG)refs;
}

/* Environment ready: create the controller on the host window. */
static HRESULT STDMETHODCALLTYPE wv_env_handler_invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *result) {
    wv_env_handler *h = (wv_env_handler *)This;
    wv_ctx *ctx = h->ctx;
    if (FAILED(errorCode) || !result) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "WebView2 environment creation failed (HRESULT 0x%08lx)",
                 (unsigned long)errorCode);
        ctx->result = 2;
        DestroyWindow(ctx->hwnd);
        return S_OK;
    }
    ctx->environment = result;
    wchar_t *version = NULL;
    if (result->lpVtbl->get_BrowserVersionString(result, &version) == S_OK &&
        version && version[0]) {
        printf("OtsarDB Studio desktop window: WebView2 runtime %ls\n", version);
        fflush(stdout);
        CoTaskMemFree(version);
    }
    RECT rc;
    GetClientRect(ctx->hwnd, &rc);
    HRESULT hr = result->lpVtbl->CreateCoreWebView2Controller(
        result, ctx->hwnd,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)
            &ctx->ctrl_handler);
    if (FAILED(hr)) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "WebView2 controller creation failed (HRESULT 0x%08lx)",
                 (unsigned long)hr);
        ctx->result = 2;
        DestroyWindow(ctx->hwnd);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_ctrl_handler_qi(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    REFIID riid, void **ppvObject) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_WV2_CTRL_HANDLER)) {
        wv_ctrl_handler *h = (wv_ctrl_handler *)This;
        ++h->refs;
        *ppvObject = This;
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE wv_ctrl_handler_addref(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This) {
    wv_ctrl_handler *h = (wv_ctrl_handler *)This;
    return (ULONG)++h->refs;
}

static ULONG STDMETHODCALLTYPE wv_ctrl_handler_release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This) {
    wv_ctrl_handler *h = (wv_ctrl_handler *)This;
    return (ULONG)--h->refs;
}

/* Controller ready: wire the window, suppress popups and navigate. */
static HRESULT STDMETHODCALLTYPE wv_ctrl_handler_invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *result) {
    wv_ctrl_handler *h = (wv_ctrl_handler *)This;
    wv_ctx *ctx = h->ctx;
    if (FAILED(errorCode) || !result) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "WebView2 controller creation failed (HRESULT 0x%08lx)",
                 (unsigned long)errorCode);
        ctx->result = 2;
        DestroyWindow(ctx->hwnd);
        return S_OK;
    }
    ctx->controller = result;
    ICoreWebView2 *webview = NULL;
    HRESULT hr = result->lpVtbl->get_CoreWebView2(result, &webview);
    if (FAILED(hr) || !webview) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "WebView2 get_CoreWebView2 failed (HRESULT 0x%08lx)",
                 (unsigned long)hr);
        ctx->result = 2;
        DestroyWindow(ctx->hwnd);
        return S_OK;
    }
    ctx->webview = webview;
    result->lpVtbl->put_ParentWindow(result, ctx->hwnd);
    RECT rc;
    GetClientRect(ctx->hwnd, &rc);
    result->lpVtbl->put_Bounds(result, rc);
    result->lpVtbl->put_IsVisible(result, TRUE);
    webview->lpVtbl->add_NewWindowRequested(
        webview,
        (ICoreWebView2NewWindowRequestedEventHandler *)&ctx->new_handler,
        &ctx->new_token);
    webview->lpVtbl->add_NavigationCompleted(
        webview,
        (ICoreWebView2NavigationCompletedEventHandler *)&ctx->nav_handler,
        &ctx->nav_token);
    printf("OtsarDB Studio desktop window: loading %ls ...\n", ctx->url);
    fflush(stdout);
    webview->lpVtbl->Navigate(webview, ctx->url);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_nav_handler_qi(
    ICoreWebView2NavigationCompletedEventHandler *This, REFIID riid,
    void **ppvObject) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_WV2_NAV_EVENT)) {
        wv_nav_handler *h = (wv_nav_handler *)This;
        ++h->refs;
        *ppvObject = This;
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE wv_nav_handler_addref(
    ICoreWebView2NavigationCompletedEventHandler *This) {
    wv_nav_handler *h = (wv_nav_handler *)This;
    return (ULONG)++h->refs;
}

static ULONG STDMETHODCALLTYPE wv_nav_handler_release(
    ICoreWebView2NavigationCompletedEventHandler *This) {
    wv_nav_handler *h = (wv_nav_handler *)This;
    return (ULONG)--h->refs;
}

static HRESULT STDMETHODCALLTYPE wv_nav_handler_invoke(
    ICoreWebView2NavigationCompletedEventHandler *This, ICoreWebView2 *sender,
    ICoreWebView2NavigationCompletedEventArgs *args) {
    (void)sender;
    wv_nav_handler *h = (wv_nav_handler *)This;
    (void)h;
    BOOL ok = FALSE;
    if (args && args->lpVtbl->get_IsSuccess(args, &ok) == S_OK && ok) {
        printf("OtsarDB Studio desktop window: page loaded.\n");
        fflush(stdout);
    } else {
        fprintf(stderr, "otsardb: studio window: page navigation failed "
                        "(is the server still listening?)\n");
        fflush(stderr);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_new_handler_qi(
    ICoreWebView2NewWindowRequestedEventHandler *This, REFIID riid,
    void **ppvObject) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_WV2_NEW_EVENT)) {
        wv_new_handler *h = (wv_new_handler *)This;
        ++h->refs;
        *ppvObject = This;
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE wv_new_handler_addref(
    ICoreWebView2NewWindowRequestedEventHandler *This) {
    wv_new_handler *h = (wv_new_handler *)This;
    return (ULONG)++h->refs;
}

static ULONG STDMETHODCALLTYPE wv_new_handler_release(
    ICoreWebView2NewWindowRequestedEventHandler *This) {
    wv_new_handler *h = (wv_new_handler *)This;
    return (ULONG)--h->refs;
}

/* Single-window workbench: target=_blank style popups are suppressed
 * (handled = TRUE), the studio UI never spawns a second window. */
static HRESULT STDMETHODCALLTYPE wv_new_handler_invoke(
    ICoreWebView2NewWindowRequestedEventHandler *This, ICoreWebView2 *sender,
    ICoreWebView2NewWindowRequestedEventArgs *args) {
    (void)sender;
    wv_new_handler *h = (wv_new_handler *)This;
    (void)h;
    if (args) args->lpVtbl->put_Handled(args, TRUE);
    return S_OK;
}

/* ------------------------------------------------------------ window */

static LRESULT CALLBACK wv_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    wv_ctx *ctx = (wv_ctx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_SIZE:
        if (ctx && ctx->controller) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            ctx->controller->lpVtbl->put_Bounds(ctx->controller, rc);
        }
        return 0;
    case WM_CLOSE:
        /* User closed the window: hide and close the WebView2 controller,
         * then destroy the host window (WM_DESTROY ends the message loop). */
        if (ctx && ctx->controller) {
            ctx->controller->lpVtbl->put_IsVisible(ctx->controller, FALSE);
            ctx->controller->lpVtbl->Close(ctx->controller);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (ctx) ctx->result = 0; /* clean window close */
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The window body: creates the host window, starts the WebView2
 * environment creation, runs the message loop until the window closes,
 * releases everything. Returns 0 on clean close, 2 on setup failure. */
static int wv_open_window(wv_ctx *ctx) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "COM initialization failed (HRESULT 0x%08lx)", (unsigned long)hr);
        return 2;
    }

    static int class_registered = 0;
    if (!class_registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wv_wnd_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"OtsarDBStudioWindow";
        if (!RegisterClassExW(&wc)) {
            snprintf(ctx->error, sizeof(ctx->error),
                     "cannot register the window class (error %lu)",
                     (unsigned long)GetLastError());
            CoUninitialize();
            return 2;
        }
        class_registered = 1;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    int w = 1280, h = 820;
    RECT wr = {0, 0, w, h};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    ctx->hwnd = CreateWindowExW(
        0, L"OtsarDBStudioWindow", ctx->url, style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandleW(NULL), ctx);
    if (!ctx->hwnd) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "cannot create the window (error %lu)",
                 (unsigned long)GetLastError());
        CoUninitialize();
        return 2;
    }
    ShowWindow(ctx->hwnd, SW_SHOW);
    UpdateWindow(ctx->hwnd);

    wv2_create_environment_fn create_env = (wv2_create_environment_fn)
        GetProcAddress(ctx->loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!create_env) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "WebView2Loader.dll does not export "
                 "CreateCoreWebView2EnvironmentWithOptions");
        ctx->result = 2;
        DestroyWindow(ctx->hwnd);
    } else {
        hr = create_env(NULL, NULL, NULL,
                        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)
                            &ctx->env_handler);
        if (FAILED(hr)) {
            snprintf(ctx->error, sizeof(ctx->error),
                     "CreateCoreWebView2EnvironmentWithOptions failed "
                     "(HRESULT 0x%08lx)",
                     (unsigned long)hr);
            ctx->result = 2;
            DestroyWindow(ctx->hwnd);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ctx->controller) {
        ctx->controller->lpVtbl->put_IsVisible(ctx->controller, FALSE);
        ctx->controller->lpVtbl->Close(ctx->controller);
        if (ctx->webview) ctx->webview->lpVtbl->Release(ctx->webview);
        ctx->controller->lpVtbl->Release(ctx->controller);
        ctx->controller = NULL;
        ctx->webview = NULL;
    }
    if (ctx->environment) {
        ctx->environment->lpVtbl->Release(ctx->environment);
        ctx->environment = NULL;
    }
    if (ctx->loader) {
        FreeLibrary(ctx->loader);
        ctx->loader = NULL;
    }
    CoUninitialize();
    return ctx->result;
}

/* -------------------------------------------------------------- entry */

int otsardb_studio_window_open(int port, char *error, size_t error_cap) {
    if (error && error_cap) error[0] = '\0';

    wchar_t loader_path[1024];
    if (!wv_find_loader(loader_path, sizeof(loader_path) / sizeof(wchar_t))) {
        if (error && error_cap) {
            snprintf(error, error_cap,
                     "WebView2Loader.dll was not found (searched the WebView2 "
                     "runtime directories under Program Files and %%LOCALAPPDATA%%, "
                     "the executable directory and the vcpkg store); the desktop "
                     "window is unavailable");
        }
        return 1;
    }

    HMODULE loader = LoadLibraryW(loader_path);
    if (!loader) {
        if (error && error_cap) {
            snprintf(error, error_cap,
                     "WebView2Loader.dll was found at %ls but could not be "
                     "loaded (error %lu)",
                     loader_path, (unsigned long)GetLastError());
        }
        return 2;
    }

    wv_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = 2;
    ctx.loader = loader;
    _snwprintf_s(ctx.url, 128, _TRUNCATE, L"http://127.0.0.1:%d/", port);

    static const ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_vtbl = {
        wv_env_handler_qi, wv_env_handler_addref, wv_env_handler_release,
        wv_env_handler_invoke};
    static const ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ctrl_vtbl = {
        wv_ctrl_handler_qi, wv_ctrl_handler_addref, wv_ctrl_handler_release,
        wv_ctrl_handler_invoke};
    static const ICoreWebView2NavigationCompletedEventHandlerVtbl nav_vtbl = {
        wv_nav_handler_qi, wv_nav_handler_addref, wv_nav_handler_release,
        wv_nav_handler_invoke};
    static const ICoreWebView2NewWindowRequestedEventHandlerVtbl new_vtbl = {
        wv_new_handler_qi, wv_new_handler_addref, wv_new_handler_release,
        wv_new_handler_invoke};
    ctx.env_handler.lpVtbl = (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *)&env_vtbl;
    ctx.ctrl_handler.lpVtbl = (ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *)&ctrl_vtbl;
    ctx.nav_handler.lpVtbl = (ICoreWebView2NavigationCompletedEventHandlerVtbl *)&nav_vtbl;
    ctx.new_handler.lpVtbl = (ICoreWebView2NewWindowRequestedEventHandlerVtbl *)&new_vtbl;
    ctx.env_handler.refs = 1;
    ctx.ctrl_handler.refs = 1;
    ctx.nav_handler.refs = 1;
    ctx.new_handler.refs = 1;
    ctx.env_handler.ctx = &ctx;
    ctx.ctrl_handler.ctx = &ctx;
    ctx.nav_handler.ctx = &ctx;
    ctx.new_handler.ctx = &ctx;

    int rc = wv_open_window(&ctx);
    if (rc != 0 && error && error_cap) {
        snprintf(error, error_cap, "%s", ctx.error[0] ? ctx.error
                                                       : "desktop window failed");
    }
    return rc;
}

#else /* !defined(_WIN32) — POSIX stub: the window is Windows-only and the
       * caller falls back to the v0.5 default-browser behavior. */

int otsardb_studio_window_open(int port, char *error, size_t error_cap) {
    (void)port;
    if (error && error_cap) {
        snprintf(error, error_cap,
                 "the desktop window (WebView2) is only available on Windows");
    }
    return 1;
}

#endif
