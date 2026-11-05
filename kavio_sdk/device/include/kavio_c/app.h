#ifndef KAVIO_C_APP_H_
#define KAVIO_C_APP_H_

/** C ABI for application lifecycle and callbacks. */

#include <kavio_c/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KavioApp KavioApp;
typedef struct KavioPresentation KavioPresentation;
typedef struct KavioDevice KavioDevice;

typedef struct KavioAppOptions {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* client_name;
    uint32_t idle_sleep_ms;
    uint8_t install_signal_handlers;
    uint8_t reserved[3];

    /* Device Session options. Readers must honor struct_size before accessing them. */
    uint8_t rx_enabled;
    uint8_t tx_enabled;
    uint8_t reserved2[2];
    uint32_t rx_frame_sample_interval;
    int32_t tx_z_order;
} KavioAppOptions;

typedef void (*KavioAppReadyCallback)(KavioApp* app, void* user_data);
typedef void (*KavioAppTickCallback)(KavioApp* app, void* user_data);
typedef void (*KavioAppStoppingCallback)(KavioApp* app, void* user_data);

#define KAVIO_APP_OPTIONS_V1_SIZE \
    (offsetof(KavioAppOptions, reserved) + sizeof(((KavioAppOptions*)0)->reserved))

#define KAVIO_APP_OPTIONS_DEVICE_V1_SIZE \
    (offsetof(KavioAppOptions, tx_z_order) + sizeof(((KavioAppOptions*)0)->tx_z_order))

#define KAVIO_APP_OPTIONS_INIT \
    { sizeof(KavioAppOptions), KAVIO_ABI_VERSION, NULL, 5u, 1u, {0u, 0u, 0u}, 0u, 0u, {0u, 0u}, 1u, 0 }

/** Initialize options with ABI-v1 defaults. */
KAVIO_APP_API void kavio_app_options_init(KavioAppOptions* options);

/** Create one App handle. The caller owns the returned handle. */
KAVIO_APP_API KavioStatusCode kavio_app_create(const KavioAppOptions* options, KavioApp** out_app);
/**
 * Set the ready callback. It runs on the same thread that executes kavio_app_run(),
 * after the AppView Presentation is ready and the initial frame is published.
 */
KAVIO_APP_API KavioStatusCode kavio_app_set_ready_callback(
    KavioApp* app,
    KavioAppReadyCallback callback,
    void* user_data);
KAVIO_APP_API KavioStatusCode kavio_app_set_tick_callback(
    KavioApp* app,
    KavioAppTickCallback callback,
    void* user_data);
KAVIO_APP_API KavioStatusCode kavio_app_set_stopping_callback(
    KavioApp* app,
    KavioAppStoppingCallback callback,
    void* user_data);
/**
 * Return the App-owned Presentation handle. The handle is non-owning and remains
 * valid until kavio_app_destroy().
 */
KAVIO_APP_API KavioStatusCode kavio_app_get_presentation(
    KavioApp* app,
    KavioPresentation** out_presentation);
KAVIO_APP_API KavioStatusCode kavio_app_get_device(
    KavioApp* app,
    KavioDevice** out_device);
/** Blocking run loop. May be called only once at a time for an App handle. */
KAVIO_APP_API KavioStatusCode kavio_app_run(KavioApp* app, int32_t* out_exit_code);
/** Thread-safe stop request. */
KAVIO_APP_API KavioStatusCode kavio_app_request_stop(KavioApp* app);
KAVIO_APP_API KavioStatusCode kavio_app_last_status(KavioApp* app);
/** Destroy an App after kavio_app_run() has returned. */
KAVIO_APP_API KavioStatusCode kavio_app_destroy(KavioApp* app);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_APP_H_
