#ifndef KAVIO_C_PRESENTATION_H_
#define KAVIO_C_PRESENTATION_H_

/** C ABI for presentation creation and display access. */

#include <kavio_c/events.h>
#include <kavio_c/framebuffer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KavioPresentation KavioPresentation;

KAVIO_APP_API uint8_t kavio_presentation_is_ready(KavioPresentation* presentation);
KAVIO_APP_API uint8_t kavio_presentation_is_visible(KavioPresentation* presentation);
KAVIO_APP_API uint64_t kavio_presentation_generation(KavioPresentation* presentation);
KAVIO_APP_API KavioStatusCode kavio_presentation_get_frame_info(
    KavioPresentation* presentation,
    KavioFrameInfo* out_info);
KAVIO_APP_API KavioStatusCode kavio_presentation_acquire_frame(
    KavioPresentation* presentation,
    KavioFrameLease** out_frame);
KAVIO_APP_API KavioStatusCode kavio_presentation_clear(
    KavioPresentation* presentation,
    uint8_t blue,
    uint8_t green,
    uint8_t red,
    uint8_t alpha);
/** Touch callback runs on the App run thread; frame point memory is callback-scoped. */
KAVIO_APP_API KavioStatusCode kavio_presentation_set_touch_callback(
    KavioPresentation* presentation,
    KavioTouchCallback callback,
    void* user_data);
/** Visibility callback runs on the App run thread. */
KAVIO_APP_API KavioStatusCode kavio_presentation_set_visibility_callback(
    KavioPresentation* presentation,
    KavioVisibilityCallback callback,
    void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_PRESENTATION_H_
