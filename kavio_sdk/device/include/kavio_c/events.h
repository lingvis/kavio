#ifndef KAVIO_C_EVENTS_H_
#define KAVIO_C_EVENTS_H_

/** C-compatible input and event data structures. */

#include <kavio_c/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KavioPresentation KavioPresentation;

typedef struct KavioTouchPoint {
    int32_t slot;
    int32_t tracking_id;
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t touch_major;
    uint8_t active;
    uint8_t changed;
    uint8_t reserved[2];
} KavioTouchPoint;

typedef struct KavioTouchFrame {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t surface_id;
    uint32_t point_count;
    uint64_t sequence;
    const KavioTouchPoint* points;
    uint8_t cancelled;
    uint8_t reserved[7];
} KavioTouchFrame;

typedef void (*KavioTouchCallback)(
    KavioPresentation* presentation,
    const KavioTouchFrame* frame,
    void* user_data);

typedef void (*KavioVisibilityCallback)(
    KavioPresentation* presentation,
    uint8_t visible,
    void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_EVENTS_H_
