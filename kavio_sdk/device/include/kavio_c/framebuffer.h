#ifndef KAVIO_C_FRAMEBUFFER_H_
#define KAVIO_C_FRAMEBUFFER_H_

/** C ABI for writable framebuffer leases and updates. */

#include <kavio_c/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KavioFrameLease KavioFrameLease;

typedef struct KavioFrameInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint64_t buffer_size;
    uint32_t surface_id;
    uint32_t reserved0;
    uint64_t generation;
} KavioFrameInfo;

#define KAVIO_FRAME_INFO_V1_SIZE \
    (offsetof(KavioFrameInfo, generation) + sizeof(((KavioFrameInfo*)0)->generation))

#define KAVIO_FRAME_INFO_INIT \
    { sizeof(KavioFrameInfo), KAVIO_ABI_VERSION, 0u, 0u, 0u, KAVIO_PIXEL_FORMAT_UNKNOWN, 0u, 0u, 0u, 0u }

/** Initialize a FrameInfo output structure before passing it to the ABI. */
KAVIO_APP_API void kavio_frame_info_init(KavioFrameInfo* info);
KAVIO_APP_API KavioStatusCode kavio_frame_get_info(KavioFrameLease* frame, KavioFrameInfo* out_info);
/**
 * Return writable SDK-owned pixel memory. The pointer is valid only while the
 * FrameLease remains active and must not be retained after Present/Discard/Destroy.
 */
KAVIO_APP_API KavioStatusCode kavio_frame_get_pixels(
    KavioFrameLease* frame,
    void** out_pixels,
    uint64_t* out_buffer_size);
/**
 * Publish the frame and finish the write transaction. dirty_rect_count==0 means
 * full-frame publication. Present is asynchronous and is not a VSync fence.
 */
KAVIO_APP_API KavioStatusCode kavio_frame_present(
    KavioFrameLease* frame,
    const KavioRect* dirty_rects,
    uint32_t dirty_rect_count);
KAVIO_APP_API KavioStatusCode kavio_frame_discard(KavioFrameLease* frame);
/**
 * Destroy the lease. An active lease is discarded first. Active leases must be
 * destroyed on the thread that acquired them.
 */
KAVIO_APP_API KavioStatusCode kavio_frame_destroy(KavioFrameLease* frame);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_FRAMEBUFFER_H_
