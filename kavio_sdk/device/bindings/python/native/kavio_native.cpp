#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <kavio_c/app.h>
#include <kavio_c/device.h>
#include <kavio_c/presentation.h>
#include <kavio/Keyboard.h>

#ifndef PyCFunction_CAST
#define PyCFunction_CAST(func) reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(func))
#endif

namespace {

PyObject* g_error = nullptr;

bool RaiseStatus(KavioStatusCode status)
{
    if (status == KAVIO_STATUS_OK) return true;
    const char* message = kavio_last_error_message();
    PyErr_Format(g_error ? g_error : PyExc_RuntimeError,
                 "Kavio error %d: %s", static_cast<int>(status),
                 message && *message ? message : kavio_status_code_name(status));
    return false;
}

struct AppState {
    KavioApp* app = nullptr;
    PyObject* ready = nullptr;
    PyObject* tick = nullptr;
    PyObject* stopping = nullptr;
    PyObject* callback_type = nullptr;
    PyObject* callback_value = nullptr;
    PyObject* callback_traceback = nullptr;
};

void ClearCallbackException(AppState* state)
{
    if (!state) return;
    Py_XDECREF(state->callback_type);
    Py_XDECREF(state->callback_value);
    Py_XDECREF(state->callback_traceback);
    state->callback_type = nullptr;
    state->callback_value = nullptr;
    state->callback_traceback = nullptr;
}

void CaptureCallbackException(AppState* state)
{
    if (!state || !PyErr_Occurred()) return;
    if (!state->callback_type) {
        PyErr_Fetch(&state->callback_type, &state->callback_value, &state->callback_traceback);
        PyErr_NormalizeException(&state->callback_type, &state->callback_value, &state->callback_traceback);
    } else {
        PyErr_Clear();
    }
    if (state->app) (void)kavio_app_request_stop(state->app);
}

void CallPythonCallback(AppState* state, PyObject* callback)
{
    if (!state || !callback || callback == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunctionObjArgs(callback, nullptr);
    if (!result) CaptureCallbackException(state);
    Py_XDECREF(result);
    PyGILState_Release(gil);
}

void ReadyBridge(KavioApp*, void* user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    CallPythonCallback(state, state ? state->ready : nullptr);
}
void TickBridge(KavioApp*, void* user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    CallPythonCallback(state, state ? state->tick : nullptr);
}
void StoppingBridge(KavioApp*, void* user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    CallPythonCallback(state, state ? state->stopping : nullptr);
}

void ReplaceCallback(PyObject*& slot, PyObject* callback)
{
    Py_XINCREF(callback);
    Py_XDECREF(slot);
    slot = callback;
}

AppState* GetApp(PyObject* capsule)
{
    return static_cast<AppState*>(PyCapsule_GetPointer(capsule, "kavio.app"));
}

void CaptureOwnedCallbackException(PyObject* owner)
{
    if (!PyErr_Occurred()) return;
    if (!owner) { PyErr_Clear(); return; }
    auto* state = static_cast<AppState*>(PyCapsule_GetPointer(owner, "kavio.app"));
    if (!state) { PyErr_Clear(); return; }
    CaptureCallbackException(state);
}

void AppCapsuleDestroy(PyObject* capsule)
{
    auto* state = static_cast<AppState*>(PyCapsule_GetPointer(capsule, "kavio.app"));
    if (!state) { PyErr_Clear(); return; }
    if (state->app) {
        (void)kavio_app_set_ready_callback(state->app, nullptr, nullptr);
        (void)kavio_app_set_tick_callback(state->app, nullptr, nullptr);
        (void)kavio_app_set_stopping_callback(state->app, nullptr, nullptr);
        (void)kavio_app_destroy(state->app);
    }
    Py_XDECREF(state->ready);
    Py_XDECREF(state->tick);
    Py_XDECREF(state->stopping);
    ClearCallbackException(state);
    delete state;
}

struct PresentationRef {
    KavioPresentation* presentation = nullptr;
    PyObject* owner = nullptr;
    PyObject* touch = nullptr;
    PyObject* visibility = nullptr;
};

PresentationRef* GetPresentation(PyObject* capsule)
{
    return static_cast<PresentationRef*>(PyCapsule_GetPointer(capsule, "kavio.presentation"));
}

void PresentationCapsuleDestroy(PyObject* capsule)
{
    auto* ref = static_cast<PresentationRef*>(PyCapsule_GetPointer(capsule, "kavio.presentation"));
    if (!ref) { PyErr_Clear(); return; }
    if (ref->presentation) {
        (void)kavio_presentation_set_touch_callback(ref->presentation, nullptr, nullptr);
        (void)kavio_presentation_set_visibility_callback(ref->presentation, nullptr, nullptr);
    }
    Py_XDECREF(ref->touch);
    Py_XDECREF(ref->visibility);
    Py_XDECREF(ref->owner);
    delete ref;
}

void TouchBridge(KavioPresentation*, const KavioTouchFrame* frame, void* user_data)
{
    auto* ref = static_cast<PresentationRef*>(user_data);
    if (!ref || !ref->touch || ref->touch == Py_None || !frame) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* points = PyList_New(frame->point_count);
    if (!points) { CaptureOwnedCallbackException(ref->owner); PyGILState_Release(gil); return; }
    for (uint32_t i = 0; i < frame->point_count; ++i) {
        const auto& p = frame->points[i];
        PyObject* item = Py_BuildValue(
            "{s:i,s:i,s:i,s:i,s:i,s:i,s:O,s:O}",
            "slot", p.slot, "tracking_id", p.tracking_id,
            "x", p.x, "y", p.y, "pressure", p.pressure,
            "touch_major", p.touch_major,
            "active", p.active ? Py_True : Py_False,
            "changed", p.changed ? Py_True : Py_False);
        PyList_SET_ITEM(points, i, item);
    }
    PyObject* event = Py_BuildValue(
        "{s:I,s:K,s:O,s:O}",
        "surface_id", frame->surface_id,
        "sequence", static_cast<unsigned long long>(frame->sequence),
        "points", points,
        "cancelled", frame->cancelled ? Py_True : Py_False);
    Py_DECREF(points);
    if (event) {
        PyObject* result = PyObject_CallFunctionObjArgs(ref->touch, event, nullptr);
        if (!result) CaptureOwnedCallbackException(ref->owner);
        Py_XDECREF(result);
        Py_DECREF(event);
    } else {
        CaptureOwnedCallbackException(ref->owner);
    }
    PyGILState_Release(gil);
}

void VisibilityBridge(KavioPresentation*, uint8_t visible, void* user_data)
{
    auto* ref = static_cast<PresentationRef*>(user_data);
    if (!ref || !ref->visibility || ref->visibility == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* arg = visible ? Py_True : Py_False;
    Py_INCREF(arg);
    PyObject* result = PyObject_CallFunctionObjArgs(ref->visibility, arg, nullptr);
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result);
    Py_DECREF(arg);
    PyGILState_Release(gil);
}

struct DeviceRef {
    KavioDevice* device = nullptr;
    PyObject* owner = nullptr;
    PyObject* comm = nullptr;
    PyObject* tx_surface = nullptr;
    PyObject* h264_stream = nullptr;
    PyObject* h264_invalidated = nullptr;
    PyObject* error = nullptr;
    PyObject* config_changed = nullptr;
};

DeviceRef* GetDevice(PyObject* capsule)
{
    return static_cast<DeviceRef*>(PyCapsule_GetPointer(capsule, "kavio.device"));
}

void DeviceCapsuleDestroy(PyObject* capsule)
{
    auto* ref = static_cast<DeviceRef*>(PyCapsule_GetPointer(capsule, "kavio.device"));
    if (!ref) { PyErr_Clear(); return; }
    if (ref->device) {
        (void)kavio_device_set_comm_data_callback(ref->device, nullptr, nullptr);
        (void)kavio_device_set_tx_surface_callback(ref->device, nullptr, nullptr);
        (void)kavio_device_set_h264_stream_callback(ref->device, nullptr, nullptr);
        (void)kavio_device_set_h264_invalidated_callback(ref->device, nullptr, nullptr);
        (void)kavio_device_set_error_callback(ref->device, nullptr, nullptr);
        (void)kavio_device_set_config_changed_callback(ref->device, nullptr, nullptr);
    }
    Py_XDECREF(ref->comm);
    Py_XDECREF(ref->tx_surface);
    Py_XDECREF(ref->h264_stream);
    Py_XDECREF(ref->h264_invalidated);
    Py_XDECREF(ref->error);
    Py_XDECREF(ref->config_changed);
    Py_XDECREF(ref->owner);
    delete ref;
}

// ---------------- FrameLease Python object ----------------

typedef struct {
    PyObject_HEAD
    KavioFrameLease* frame;
    Py_ssize_t export_count;
} PyFrameLease;

PyTypeObject g_frame_type = {PyVarObject_HEAD_INIT(nullptr, 0)};

void FrameDealloc(PyFrameLease* self)
{
    if (self->frame) (void)kavio_frame_destroy(self->frame);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* FrameInfo(PyFrameLease* self, PyObject*)
{
    KavioFrameInfo info = KAVIO_FRAME_INFO_INIT;
    if (!RaiseStatus(kavio_frame_get_info(self->frame, &info))) return nullptr;
    return Py_BuildValue(
        "{s:I,s:I,s:I,s:I,s:K,s:I,s:K}",
        "width", info.width, "height", info.height, "stride", info.stride,
        "pixel_format", info.pixel_format,
        "buffer_size", static_cast<unsigned long long>(info.buffer_size),
        "surface_id", info.surface_id,
        "generation", static_cast<unsigned long long>(info.generation));
}

bool ParseRectSequence(PyObject* object, std::vector<KavioRect>& rects)
{
    if (!object || object == Py_None) return true;
    PyObject* seq = PySequence_Fast(object, "dirty_rects must be a sequence of 4-item sequences");
    if (!seq) return false;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
    rects.reserve(static_cast<size_t>(count));
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = PySequence_Fast(PySequence_Fast_GET_ITEM(seq, i), "dirty rect must be a sequence");
        if (!item) { Py_DECREF(seq); return false; }
        if (PySequence_Fast_GET_SIZE(item) != 4) {
            Py_DECREF(item); Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError, "dirty rect must contain x, y, width, height");
            return false;
        }
        long x = PyLong_AsLong(PySequence_Fast_GET_ITEM(item, 0));
        long y = PyLong_AsLong(PySequence_Fast_GET_ITEM(item, 1));
        unsigned long w = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(item, 2));
        unsigned long h = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(item, 3));
        Py_DECREF(item);
        if (PyErr_Occurred()) { Py_DECREF(seq); return false; }
        rects.push_back(KavioRect{static_cast<int32_t>(x), static_cast<int32_t>(y),
                                  static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
    }
    Py_DECREF(seq);
    return true;
}

PyObject* FramePresent(PyFrameLease* self, PyObject* args)
{
    if (self->export_count != 0) {
        PyErr_SetString(PyExc_BufferError, "release FrameBuffer views before present()");
        return nullptr;
    }
    PyObject* dirty = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &dirty)) return nullptr;
    std::vector<KavioRect> rects;
    if (!ParseRectSequence(dirty, rects)) return nullptr;
    if (!RaiseStatus(kavio_frame_present(self->frame,
                                         rects.empty() ? nullptr : rects.data(),
                                         static_cast<uint32_t>(rects.size())))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* FrameDiscard(PyFrameLease* self, PyObject*)
{
    if (self->export_count != 0) {
        PyErr_SetString(PyExc_BufferError, "release FrameBuffer views before discard()");
        return nullptr;
    }
    if (!RaiseStatus(kavio_frame_discard(self->frame))) return nullptr;
    Py_RETURN_NONE;
}

int FrameGetBuffer(PyObject* object, Py_buffer* view, int flags)
{
    auto* self = reinterpret_cast<PyFrameLease*>(object);
    void* pixels = nullptr;
    uint64_t size = 0;
    if (!RaiseStatus(kavio_frame_get_pixels(self->frame, &pixels, &size))) return -1;
    if (size > static_cast<uint64_t>(PY_SSIZE_T_MAX)) {
        PyErr_SetString(PyExc_OverflowError, "FrameBuffer is too large for Python buffer protocol");
        return -1;
    }
    const int result = PyBuffer_FillInfo(
        view, object, pixels, static_cast<Py_ssize_t>(size), 0, flags);
    if (result == 0) ++self->export_count;
    return result;
}

void FrameReleaseBuffer(PyObject* object, Py_buffer*)
{
    auto* self = reinterpret_cast<PyFrameLease*>(object);
    if (self->export_count > 0) --self->export_count;
}

PyMethodDef kFrameMethods[] = {
    {"info", reinterpret_cast<PyCFunction>(FrameInfo), METH_NOARGS, nullptr},
    {"present", reinterpret_cast<PyCFunction>(FramePresent), METH_VARARGS, nullptr},
    {"discard", reinterpret_cast<PyCFunction>(FrameDiscard), METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};
PyBufferProcs kFrameBuffer = {FrameGetBuffer, FrameReleaseBuffer};

PyObject* NewFrameObject(KavioFrameLease* frame)
{
    auto* object = PyObject_New(PyFrameLease, &g_frame_type);
    if (!object) { (void)kavio_frame_destroy(frame); return nullptr; }
    object->frame = frame;
    object->export_count = 0;
    return reinterpret_cast<PyObject*>(object);
}

// ---------------- RX ImageFrame Python object ----------------

typedef struct {
    PyObject_HEAD
    KavioImageFrame* frame;
} PyImageFrame;

PyTypeObject g_image_frame_type = {PyVarObject_HEAD_INIT(nullptr, 0)};

void ImageFrameDealloc(PyImageFrame* self)
{
    if (self->frame) kavio_image_frame_destroy(self->frame);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* ImageFrameInfo(PyImageFrame* self, PyObject*)
{
    KavioImageFrameInfo info{};
    kavio_image_frame_info_init(&info);
    if (!RaiseStatus(kavio_image_frame_get_info(self->frame, &info))) return nullptr;
    PyObject* valid = Py_BuildValue(
        "(iiII)", info.valid_rect.pos_x, info.valid_rect.pos_y,
        info.valid_rect.width, info.valid_rect.height);
    if (!valid) return nullptr;
    PyObject* result = Py_BuildValue(
        "{s:K,s:K,s:I,s:I,s:I,s:I,s:i,s:i,s:O,s:K}",
        "frame_id", static_cast<unsigned long long>(info.frame_id),
        "timestamp_ns", static_cast<unsigned long long>(info.timestamp_ns),
        "width", info.width, "height", info.height,
        "stride", info.stride, "pixel_format", info.pixel_format,
        "dest_pos_x", info.dest_pos_x, "dest_pos_y", info.dest_pos_y,
        "valid_rect", valid,
        "data_size", static_cast<unsigned long long>(info.data_size));
    Py_DECREF(valid);
    return result;
}

int ImageFrameGetBuffer(PyObject* object, Py_buffer* view, int flags)
{
    auto* self = reinterpret_cast<PyImageFrame*>(object);
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    if (!RaiseStatus(kavio_image_frame_get_data(self->frame, &data, &size))) return -1;
    if (size > static_cast<uint64_t>(PY_SSIZE_T_MAX)) {
        PyErr_SetString(PyExc_OverflowError, "ImageFrame is too large for Python buffer protocol");
        return -1;
    }
    return PyBuffer_FillInfo(
        view, object, const_cast<uint8_t*>(data), static_cast<Py_ssize_t>(size), 1, flags);
}

PyMethodDef kImageFrameMethods[] = {
    {"info", reinterpret_cast<PyCFunction>(ImageFrameInfo), METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};
PyBufferProcs kImageFrameBuffer = {ImageFrameGetBuffer, nullptr};

PyObject* NewImageFrameObject(KavioImageFrame* frame)
{
    auto* object = PyObject_New(PyImageFrame, &g_image_frame_type);
    if (!object) { kavio_image_frame_destroy(frame); return nullptr; }
    object->frame = frame;
    return reinterpret_cast<PyObject*>(object);
}

// ---------------- H264Packet Python object ----------------

typedef struct {
    PyObject_HEAD
    KavioH264Packet* packet;
} PyH264Packet;

PyTypeObject g_h264_packet_type = {PyVarObject_HEAD_INIT(nullptr, 0)};

void H264PacketDealloc(PyH264Packet* self)
{
    if (self->packet) kavio_h264_packet_destroy(self->packet);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* H264PacketInfo(PyH264Packet* self, PyObject*)
{
    KavioH264PacketInfo info{};
    kavio_h264_packet_info_init(&info);
    if (!RaiseStatus(kavio_h264_packet_get_info(self->packet, &info))) return nullptr;
    return Py_BuildValue(
        "{s:K,s:K,s:K,s:K,s:I,s:I,s:K}",
        "seq", static_cast<unsigned long long>(info.seq),
        "pts_us", static_cast<unsigned long long>(info.pts_us),
        "dts_us", static_cast<unsigned long long>(info.dts_us),
        "timestamp_ns", static_cast<unsigned long long>(info.timestamp_ns),
        "flags", info.flags, "nal_type", info.nal_type,
        "data_size", static_cast<unsigned long long>(info.data_size));
}

int H264PacketGetBuffer(PyObject* object, Py_buffer* view, int flags)
{
    auto* self = reinterpret_cast<PyH264Packet*>(object);
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    if (!RaiseStatus(kavio_h264_packet_get_data(self->packet, &data, &size))) return -1;
    if (size > static_cast<uint64_t>(PY_SSIZE_T_MAX)) {
        PyErr_SetString(PyExc_OverflowError, "H264Packet is too large for Python buffer protocol");
        return -1;
    }
    return PyBuffer_FillInfo(
        view, object, const_cast<uint8_t*>(data), static_cast<Py_ssize_t>(size), 1, flags);
}

PyMethodDef kH264PacketMethods[] = {
    {"info", reinterpret_cast<PyCFunction>(H264PacketInfo), METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};
PyBufferProcs kH264PacketBuffer = {H264PacketGetBuffer, nullptr};

PyObject* NewH264PacketObject(KavioH264Packet* packet)
{
    auto* object = PyObject_New(PyH264Packet, &g_h264_packet_type);
    if (!object) { kavio_h264_packet_destroy(packet); return nullptr; }
    object->packet = packet;
    return reinterpret_cast<PyObject*>(object);
}

// ---------------- TX Surface Python object ----------------

typedef struct {
    PyObject_HEAD
    KavioTxSurface* surface;
} PyTxSurface;

PyTypeObject g_tx_surface_type = {PyVarObject_HEAD_INIT(nullptr, 0)};

void TxSurfaceDealloc(PyTxSurface* self)
{
    if (self->surface) kavio_tx_surface_destroy(self->surface);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* TxSurfaceInfo(PyTxSurface* self, PyObject*)
{
    KavioTxSurfaceInfo info{};
    kavio_tx_surface_info_init(&info);
    if (!RaiseStatus(kavio_tx_surface_get_info(self->surface, &info))) return nullptr;
    return Py_BuildValue("{s:I,s:I,s:I,s:I,s:I}",
                         "surface_id", info.surface_id, "width", info.width,
                         "height", info.height, "stride", info.stride,
                         "pixel_format", info.pixel_format);
}

PyObject* TxSurfaceClear(PyTxSurface* self, PyObject*)
{
    if (!RaiseStatus(kavio_tx_surface_clear(self->surface))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* TxSurfaceRequestDestroy(PyTxSurface* self, PyObject*)
{
    uint64_t request_id = 0;
    if (!RaiseStatus(kavio_tx_surface_request_destroy(self->surface, &request_id))) return nullptr;
    return PyLong_FromUnsignedLongLong(request_id);
}

PyObject* TxSurfaceWrite(PyTxSurface* self, PyObject* args, PyObject* kwargs)
{
    static const char* names[] = {
        "data", "width", "height", "stride", "pixel_format",
        "dest_pos_x", "dest_pos_y", "valid_rect", nullptr};
    PyObject* data_object = nullptr;
    unsigned int width = 0, height = 0, stride = 0, pixel_format = 0;
    int dest_x = 0, dest_y = 0;
    PyObject* valid_rect_object = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OIIII|iiO",
                                     const_cast<char**>(names),
                                     &data_object, &width, &height, &stride, &pixel_format,
                                     &dest_x, &dest_y, &valid_rect_object)) return nullptr;
    Py_buffer buffer{};
    if (PyObject_GetBuffer(data_object, &buffer, PyBUF_CONTIG_RO) != 0) return nullptr;
    KavioRect rect{};
    KavioRect* rect_ptr = nullptr;
    if (valid_rect_object != Py_None) {
        PyObject* seq = PySequence_Fast(valid_rect_object, "valid_rect must contain x, y, width, height");
        if (!seq) { PyBuffer_Release(&buffer); return nullptr; }
        if (PySequence_Fast_GET_SIZE(seq) != 4) {
            Py_DECREF(seq);
            PyBuffer_Release(&buffer);
            PyErr_SetString(PyExc_ValueError, "valid_rect must contain x, y, width, height");
            return nullptr;
        }
        long x = PyLong_AsLong(PySequence_Fast_GET_ITEM(seq, 0));
        long y = PyLong_AsLong(PySequence_Fast_GET_ITEM(seq, 1));
        unsigned long w = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(seq, 2));
        unsigned long h = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(seq, 3));
        Py_DECREF(seq);
        if (PyErr_Occurred()) { PyBuffer_Release(&buffer); return nullptr; }
        rect = KavioRect{static_cast<int32_t>(x), static_cast<int32_t>(y),
                         static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
        rect_ptr = &rect;
    }
    const KavioStatusCode status = kavio_tx_surface_write(
        self->surface, buffer.buf, static_cast<uint64_t>(buffer.len),
        width, height, stride, pixel_format, dest_x, dest_y, rect_ptr);
    PyBuffer_Release(&buffer);
    if (!RaiseStatus(status)) return nullptr;
    Py_RETURN_NONE;
}

PyMethodDef kTxSurfaceMethods[] = {
    {"info", reinterpret_cast<PyCFunction>(TxSurfaceInfo), METH_NOARGS, nullptr},
    {"clear", reinterpret_cast<PyCFunction>(TxSurfaceClear), METH_NOARGS, nullptr},
    {"request_destroy", reinterpret_cast<PyCFunction>(TxSurfaceRequestDestroy), METH_NOARGS, nullptr},
    {"write", PyCFunction_CAST(TxSurfaceWrite), METH_VARARGS | METH_KEYWORDS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};

PyObject* NewTxSurfaceObject(KavioTxSurface* surface)
{
    auto* object = PyObject_New(PyTxSurface, &g_tx_surface_type);
    if (!object) { kavio_tx_surface_destroy(surface); return nullptr; }
    object->surface = surface;
    return reinterpret_cast<PyObject*>(object);
}

// ---------------- App module functions ----------------

PyObject* AppCreate(PyObject*, PyObject* args, PyObject* kwargs)
{
    static const char* names[] = {
        "client_name", "idle_sleep_ms", "install_signal_handlers",
        "rx_enabled", "rx_frame_sample_interval", "tx_enabled", "tx_z_order", nullptr};
    const char* client_name = nullptr;
    unsigned int idle_sleep_ms = 5;
    int signal_handlers = 1;
    int rx_enabled = 0;
    unsigned int rx_interval = 1;
    int tx_enabled = 0;
    int tx_z_order = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|zIppIpi",
                                     const_cast<char**>(names),
                                     &client_name, &idle_sleep_ms, &signal_handlers,
                                     &rx_enabled, &rx_interval, &tx_enabled, &tx_z_order)) return nullptr;
    KavioAppOptions options = KAVIO_APP_OPTIONS_INIT;
    options.client_name = client_name;
    options.idle_sleep_ms = idle_sleep_ms;
    options.install_signal_handlers = signal_handlers ? 1u : 0u;
    options.rx_enabled = rx_enabled ? 1u : 0u;
    options.rx_frame_sample_interval = rx_interval == 0 ? 1u : rx_interval;
    options.tx_enabled = tx_enabled ? 1u : 0u;
    options.tx_z_order = tx_z_order;
    auto* state = new (std::nothrow) AppState();
    if (!state) return PyErr_NoMemory();
    const KavioStatusCode status = kavio_app_create(&options, &state->app);
    if (!RaiseStatus(status)) { delete state; return nullptr; }
    (void)kavio_app_set_ready_callback(state->app, ReadyBridge, state);
    (void)kavio_app_set_tick_callback(state->app, TickBridge, state);
    (void)kavio_app_set_stopping_callback(state->app, StoppingBridge, state);
    PyObject* capsule = PyCapsule_New(state, "kavio.app", AppCapsuleDestroy);
    if (!capsule) { (void)kavio_app_destroy(state->app); delete state; return nullptr; }
    return capsule;
}

PyObject* AppSetCallback(PyObject*, PyObject* args, const char* kind)
{
    PyObject* capsule = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &capsule, &callback)) return nullptr;
    if (callback != Py_None && !PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable or None");
        return nullptr;
    }
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    if (std::strcmp(kind, "ready") == 0) ReplaceCallback(state->ready, callback);
    else if (std::strcmp(kind, "tick") == 0) ReplaceCallback(state->tick, callback);
    else ReplaceCallback(state->stopping, callback);
    Py_RETURN_NONE;
}

PyObject* AppSetReady(PyObject* self, PyObject* args) { return AppSetCallback(self, args, "ready"); }
PyObject* AppSetTick(PyObject* self, PyObject* args) { return AppSetCallback(self, args, "tick"); }
PyObject* AppSetStopping(PyObject* self, PyObject* args) { return AppSetCallback(self, args, "stopping"); }

PyObject* AppRun(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    ClearCallbackException(state);
    int32_t exit_code = 0;
    KavioStatusCode status = KAVIO_STATUS_OK;
    Py_BEGIN_ALLOW_THREADS
    status = kavio_app_run(state->app, &exit_code);
    Py_END_ALLOW_THREADS
    if (state->callback_type) {
        PyObject* type = state->callback_type;
        PyObject* value = state->callback_value;
        PyObject* traceback = state->callback_traceback;
        state->callback_type = state->callback_value = state->callback_traceback = nullptr;
        PyErr_Restore(type, value, traceback);
        return nullptr;
    }
    if (!RaiseStatus(status)) return nullptr;
    return PyLong_FromLong(exit_code);
}

PyObject* AppRequestStop(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    if (!RaiseStatus(kavio_app_request_stop(state->app))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* AppLastStatus(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    return PyLong_FromLong(kavio_app_last_status(state->app));
}

PyObject* AppPresentation(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    KavioPresentation* presentation = nullptr;
    if (!RaiseStatus(kavio_app_get_presentation(state->app, &presentation))) return nullptr;
    auto* ref = new (std::nothrow) PresentationRef();
    if (!ref) return PyErr_NoMemory();
    ref->presentation = presentation;
    ref->owner = capsule;
    Py_INCREF(capsule);
    PyObject* result = PyCapsule_New(ref, "kavio.presentation", PresentationCapsuleDestroy);
    if (!result) { Py_DECREF(capsule); delete ref; return nullptr; }
    // The UI binding is a separate Python extension module. Store the raw
    // presentation handle in the capsule context so sibling SDK extensions can
    // consume the same Presentation without depending on this module's private
    // PresentationRef wrapper layout. The capsule itself remains the owner.
    if (PyCapsule_SetContext(result, presentation) != 0) {
        Py_DECREF(result);
        return nullptr;
    }
    return result;
}

PyObject* AppDevice(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* state = GetApp(capsule);
    if (!state) return nullptr;
    KavioDevice* device = nullptr;
    if (!RaiseStatus(kavio_app_get_device(state->app, &device))) return nullptr;
    auto* ref = new (std::nothrow) DeviceRef();
    if (!ref) return PyErr_NoMemory();
    ref->device = device;
    ref->owner = capsule;
    Py_INCREF(capsule);
    PyObject* result = PyCapsule_New(ref, "kavio.device", DeviceCapsuleDestroy);
    if (!result) { Py_DECREF(capsule); delete ref; return nullptr; }
    return result;
}

// ---------------- Presentation module functions ----------------

PyObject* PresentationIsReady(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    return PyBool_FromLong(kavio_presentation_is_ready(ref->presentation));
}

PyObject* PresentationIsVisible(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    return PyBool_FromLong(kavio_presentation_is_visible(ref->presentation));
}

PyObject* PresentationGeneration(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    return PyLong_FromUnsignedLongLong(kavio_presentation_generation(ref->presentation));
}

PyObject* PresentationFrameInfo(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    KavioFrameInfo info = KAVIO_FRAME_INFO_INIT;
    if (!RaiseStatus(kavio_presentation_get_frame_info(ref->presentation, &info))) return nullptr;
    return Py_BuildValue("{s:I,s:I,s:I,s:I,s:K,s:I,s:K}",
                         "width", info.width, "height", info.height, "stride", info.stride,
                         "pixel_format", info.pixel_format,
                         "buffer_size", static_cast<unsigned long long>(info.buffer_size),
                         "surface_id", info.surface_id,
                         "generation", static_cast<unsigned long long>(info.generation));
}

PyObject* PresentationAcquireFrame(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    KavioFrameLease* frame = nullptr;
    if (!RaiseStatus(kavio_presentation_acquire_frame(ref->presentation, &frame))) return nullptr;
    return NewFrameObject(frame);
}

PyObject* PresentationClear(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    unsigned int blue=0, green=0, red=0, alpha=255;
    if (!PyArg_ParseTuple(args, "O|IIII", &capsule, &blue, &green, &red, &alpha)) return nullptr;
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    if (!RaiseStatus(kavio_presentation_clear(ref->presentation,
                                               static_cast<uint8_t>(blue), static_cast<uint8_t>(green),
                                               static_cast<uint8_t>(red), static_cast<uint8_t>(alpha)))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* PresentationSetTouch(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &capsule, &callback)) return nullptr;
    if (callback != Py_None && !PyCallable_Check(callback)) { PyErr_SetString(PyExc_TypeError, "callback must be callable or None"); return nullptr; }
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    ReplaceCallback(ref->touch, callback);
    if (!RaiseStatus(kavio_presentation_set_touch_callback(
            ref->presentation, callback == Py_None ? nullptr : TouchBridge,
            callback == Py_None ? nullptr : ref))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* PresentationSetVisibility(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &capsule, &callback)) return nullptr;
    if (callback != Py_None && !PyCallable_Check(callback)) { PyErr_SetString(PyExc_TypeError, "callback must be callable or None"); return nullptr; }
    auto* ref = GetPresentation(capsule);
    if (!ref) return nullptr;
    ReplaceCallback(ref->visibility, callback);
    if (!RaiseStatus(kavio_presentation_set_visibility_callback(
            ref->presentation, callback == Py_None ? nullptr : VisibilityBridge,
            callback == Py_None ? nullptr : ref))) return nullptr;
    Py_RETURN_NONE;
}

// ---------------- Device callbacks ----------------

void CommBridge(KavioDevice*, const uint8_t* data, uint64_t size, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->comm || ref->comm == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* bytes = PyBytes_FromStringAndSize(reinterpret_cast<const char*>(data), static_cast<Py_ssize_t>(size));
    PyObject* result = bytes ? PyObject_CallFunctionObjArgs(ref->comm, bytes, nullptr) : nullptr;
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result); Py_XDECREF(bytes);
    PyGILState_Release(gil);
}

void TxSurfaceBridge(KavioDevice*, uint64_t request_id, KavioStatusCode status,
                     KavioTxSurface* surface, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->tx_surface || ref->tx_surface == Py_None) {
        if (surface) kavio_tx_surface_destroy(surface);
        return;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* surface_object = surface ? NewTxSurfaceObject(surface) : (Py_INCREF(Py_None), Py_None);
    PyObject* result = surface_object
        ? PyObject_CallFunction(ref->tx_surface, "KiO", static_cast<unsigned long long>(request_id),
                                static_cast<int>(status), surface_object)
        : nullptr;
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result); Py_XDECREF(surface_object);
    PyGILState_Release(gil);
}

void H264StreamBridge(KavioDevice*, uint64_t request_id, KavioStatusCode status,
                      const KavioH264StreamInfo* info, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->h264_stream || ref->h264_stream == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* dict = info ? Py_BuildValue(
        "{s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
        "stream_id", info->stream_id, "width", info->width, "height", info->height,
        "fps", info->fps, "bitrate_kbps", info->bitrate_kbps, "gop", info->gop,
        "packet_capacity", info->packet_capacity, "payload_capacity", info->payload_capacity)
        : (Py_INCREF(Py_None), Py_None);
    PyObject* result = dict ? PyObject_CallFunction(ref->h264_stream, "KiO",
        static_cast<unsigned long long>(request_id), static_cast<int>(status), dict) : nullptr;
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result); Py_XDECREF(dict);
    PyGILState_Release(gil);
}

void H264InvalidatedBridge(KavioDevice*, const KavioH264InvalidatedEvent* event, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->h264_invalidated || ref->h264_invalidated == Py_None || !event) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* value = Py_BuildValue("{s:I,s:i}", "stream_id", event->stream_id, "reason", event->reason);
    PyObject* result = value ? PyObject_CallFunctionObjArgs(ref->h264_invalidated, value, nullptr) : nullptr;
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result); Py_XDECREF(value);
    PyGILState_Release(gil);
}

void DeviceErrorBridge(KavioDevice*, const KavioDeviceErrorEvent* event, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->error || ref->error == Py_None || !event) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* value = Py_BuildValue("{s:K,s:i,s:s}",
                                    "track_id", static_cast<unsigned long long>(event->track_id),
                                    "code", event->code, "message", event->message ? event->message : "");
    PyObject* result = value ? PyObject_CallFunctionObjArgs(ref->error, value, nullptr) : nullptr;
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result); Py_XDECREF(value);
    PyGILState_Release(gil);
}

void ConfigChangedBridge(KavioDevice*, void* user_data)
{
    auto* ref = static_cast<DeviceRef*>(user_data);
    if (!ref || !ref->config_changed || ref->config_changed == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunctionObjArgs(ref->config_changed, nullptr);
    if (!result) CaptureOwnedCallbackException(ref->owner);
    Py_XDECREF(result);
    PyGILState_Release(gil);
}

// ---------------- Device module functions ----------------

PyObject* DeviceBool(PyObject* args, int which)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetDevice(capsule);
    if (!ref) return nullptr;
    if (which == 0) return PyBool_FromLong(kavio_device_is_ready(ref->device));
    if (which == 1) return PyBool_FromLong(kavio_device_is_usb_host_online(ref->device));
    return PyBool_FromLong(kavio_device_is_comm_channel_ready(ref->device));
}
PyObject* DeviceIsReady(PyObject*, PyObject* args) { return DeviceBool(args,0); }
PyObject* DeviceUsbOnline(PyObject*, PyObject* args) { return DeviceBool(args,1); }
PyObject* DeviceCommReady(PyObject*, PyObject* args) { return DeviceBool(args,2); }

PyObject* DeviceChannelId(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    return PyLong_FromUnsignedLong(kavio_device_get_comm_channel_id(ref->device));
}

PyObject* DeviceRuntimeData(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    KavioRuntimeData data{}; kavio_runtime_data_init(&data);
    if(!RaiseStatus(kavio_device_get_runtime_data(ref->device,&data))) return nullptr;
    auto video=[](const KavioRuntimeVideoState& v){
        return Py_BuildValue("{s:i,s:I,s:I,s:I,s:I}","status",v.status,"width",v.width,"height",v.height,"fps",v.fps,"pixel_format",v.pixel_format);
    };
    PyObject* rx=video(data.rx); PyObject* tx=video(data.tx);
    PyObject* input=Py_BuildValue("{s:i,s:i,s:i}","mode",data.input.mode,"channel",data.input.channel,"connection_status",data.input.connection_status);
    PyObject* out=Py_BuildValue("{s:O,s:O,s:O}","rx",rx,"tx",tx,"input",input);
    Py_XDECREF(rx); Py_XDECREF(tx); Py_XDECREF(input); return out;
}

PyObject* DeviceRxInfo(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    KavioRxInfo info{}; kavio_rx_info_init(&info);
    if(!RaiseStatus(kavio_device_get_rx_info(ref->device,&info))) return nullptr;
    return Py_BuildValue("{s:O,s:O,s:I,s:I,s:I,s:I}",
                         "connected",info.connected?Py_True:Py_False,
                         "frame_available",info.frame_available?Py_True:Py_False,
                         "width",info.width,"height",info.height,"stride",info.stride,"pixel_format",info.pixel_format);
}

PyObject* DeviceSetRx(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; int enabled=0; unsigned int interval=1;
    if(!PyArg_ParseTuple(args,"Op|I",&capsule,&enabled,&interval)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if(!RaiseStatus(kavio_device_set_rx_params(ref->device,enabled?1u:0u,interval,&id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceSetTx(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; int enabled=0; int z=0;
    if(!PyArg_ParseTuple(args,"Op|i",&capsule,&enabled,&z)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if(!RaiseStatus(kavio_device_set_tx_params(ref->device,enabled?1u:0u,z,&id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceReadRx(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    auto* ref = GetDevice(capsule);
    if (!ref) return nullptr;
    KavioImageFrame* frame = nullptr;
    if (!RaiseStatus(kavio_device_read_latest_rx_frame(ref->device, &frame))) return nullptr;
    return NewImageFrameObject(frame);
}

PyObject* DeviceCreateTx(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; unsigned int w=0,h=0; if(!PyArg_ParseTuple(args,"OII",&capsule,&w,&h)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if(!RaiseStatus(kavio_device_create_tx_surface(ref->device,w,h,&id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceDestroyTx(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; unsigned int idv=0; if(!PyArg_ParseTuple(args,"OI",&capsule,&idv)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if(!RaiseStatus(kavio_device_destroy_tx_surface(ref->device,idv,&id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceSendKeyboard(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; int option=0,code=0; unsigned long long ts=0;
    if(!PyArg_ParseTuple(args,"Oii|K",&capsule,&option,&code,&ts)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    if (!RaiseStatus(kavio_device_send_keyboard(ref->device, option, code, ts))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* DeviceSendMouse(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; int option=0,x=0,y=0,wheel=0; unsigned long long ts=0;
    if(!PyArg_ParseTuple(args,"Oiiii|K",&capsule,&option,&x,&y,&wheel,&ts)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    if (!RaiseStatus(kavio_device_send_mouse(ref->device, option, x, y, wheel, ts))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* DeviceSendComm(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; PyObject* obj=nullptr; if(!PyArg_ParseTuple(args,"OO",&capsule,&obj)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; Py_buffer b{};
    if(PyObject_GetBuffer(obj,&b,PyBUF_CONTIG_RO)!=0) return nullptr;
    auto status=kavio_device_send_comm_data(ref->device,b.buf,static_cast<uint64_t>(b.len)); PyBuffer_Release(&b);
    if (!RaiseStatus(status)) return nullptr;
    Py_RETURN_NONE;
}

PyObject* DeviceGetConfig(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; char* json=nullptr; uint64_t size=0;
    if(!RaiseStatus(kavio_device_get_app_config_json(ref->device,&json,&size))) return nullptr;
    PyObject* result=PyUnicode_DecodeUTF8(json,static_cast<Py_ssize_t>(size),"strict"); kavio_free(json); return result;
}

PyObject* DeviceUpdateConfig(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; const char* json=nullptr; Py_ssize_t size=0;
    if(!PyArg_ParseTuple(args,"Os#",&capsule,&json,&size)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    if (!RaiseStatus(kavio_device_update_app_config_json(ref->device, json, static_cast<uint64_t>(size)))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* DeviceResetConfig(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr;
    if (!RaiseStatus(kavio_device_reset_app_config(ref->device))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* DeviceSubscribeH264(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; if(!PyArg_ParseTuple(args,"O",&capsule)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if (!RaiseStatus(kavio_device_subscribe_h264(ref->device, &id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceUnsubscribeH264(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; unsigned int stream=0; if(!PyArg_ParseTuple(args,"OI",&capsule,&stream)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; uint64_t id=0;
    if (!RaiseStatus(kavio_device_unsubscribe_h264(ref->device, stream, &id))) return nullptr;
    return PyLong_FromUnsignedLongLong(id);
}

PyObject* DeviceLatestH264(PyObject*, PyObject* args)
{
    PyObject* capsule=nullptr; unsigned int stream=0; if(!PyArg_ParseTuple(args,"OI",&capsule,&stream)) return nullptr;
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; return PyLong_FromUnsignedLongLong(kavio_device_get_latest_h264_packet_seq(ref->device,stream));
}

PyObject* DeviceReadH264(PyObject*, PyObject* args)
{
    PyObject* capsule = nullptr;
    unsigned int stream = 0;
    unsigned long long seq = 0;
    if (!PyArg_ParseTuple(args, "OIK", &capsule, &stream, &seq)) return nullptr;
    auto* ref = GetDevice(capsule);
    if (!ref) return nullptr;
    KavioH264Packet* packet = nullptr;
    if (!RaiseStatus(kavio_device_read_h264_packet(ref->device, stream, seq, &packet))) return nullptr;
    return NewH264PacketObject(packet);
}

PyObject* DeviceSetCallback(PyObject* args, const char* which)
{
    PyObject* capsule=nullptr; PyObject* callback=nullptr; if(!PyArg_ParseTuple(args,"OO",&capsule,&callback)) return nullptr;
    if(callback!=Py_None && !PyCallable_Check(callback)){PyErr_SetString(PyExc_TypeError,"callback must be callable or None");return nullptr;}
    auto* ref=GetDevice(capsule); if(!ref) return nullptr; KavioStatusCode status=KAVIO_STATUS_OK;
    if(std::strcmp(which,"comm")==0){ReplaceCallback(ref->comm,callback);status=kavio_device_set_comm_data_callback(ref->device,callback==Py_None?nullptr:CommBridge,callback==Py_None?nullptr:ref);}
    else if(std::strcmp(which,"tx")==0){ReplaceCallback(ref->tx_surface,callback);status=kavio_device_set_tx_surface_callback(ref->device,callback==Py_None?nullptr:TxSurfaceBridge,callback==Py_None?nullptr:ref);}
    else if(std::strcmp(which,"stream")==0){ReplaceCallback(ref->h264_stream,callback);status=kavio_device_set_h264_stream_callback(ref->device,callback==Py_None?nullptr:H264StreamBridge,callback==Py_None?nullptr:ref);}
    else if(std::strcmp(which,"invalid")==0){ReplaceCallback(ref->h264_invalidated,callback);status=kavio_device_set_h264_invalidated_callback(ref->device,callback==Py_None?nullptr:H264InvalidatedBridge,callback==Py_None?nullptr:ref);}
    else if(std::strcmp(which,"error")==0){ReplaceCallback(ref->error,callback);status=kavio_device_set_error_callback(ref->device,callback==Py_None?nullptr:DeviceErrorBridge,callback==Py_None?nullptr:ref);}
    else {ReplaceCallback(ref->config_changed,callback);status=kavio_device_set_config_changed_callback(ref->device,callback==Py_None?nullptr:ConfigChangedBridge,callback==Py_None?nullptr:ref);}
    if (!RaiseStatus(status)) return nullptr;
    Py_RETURN_NONE;
}
PyObject* DeviceSetCommCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"comm");}
PyObject* DeviceSetTxCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"tx");}
PyObject* DeviceSetStreamCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"stream");}
PyObject* DeviceSetInvalidCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"invalid");}
PyObject* DeviceSetErrorCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"error");}
PyObject* DeviceSetConfigCallback(PyObject*,PyObject* a){return DeviceSetCallback(a,"config");}

PyMethodDef kMethods[] = {
    {"app_create", PyCFunction_CAST(AppCreate), METH_VARARGS|METH_KEYWORDS, nullptr},
    {"app_set_ready_callback", AppSetReady, METH_VARARGS, nullptr},
    {"app_set_tick_callback", AppSetTick, METH_VARARGS, nullptr},
    {"app_set_stopping_callback", AppSetStopping, METH_VARARGS, nullptr},
    {"app_run", AppRun, METH_VARARGS, nullptr},
    {"app_request_stop", AppRequestStop, METH_VARARGS, nullptr},
    {"app_last_status", AppLastStatus, METH_VARARGS, nullptr},
    {"app_presentation", AppPresentation, METH_VARARGS, nullptr},
    {"app_device", AppDevice, METH_VARARGS, nullptr},
    {"presentation_is_ready", PresentationIsReady, METH_VARARGS, nullptr},
    {"presentation_is_visible", PresentationIsVisible, METH_VARARGS, nullptr},
    {"presentation_generation", PresentationGeneration, METH_VARARGS, nullptr},
    {"presentation_frame_info", PresentationFrameInfo, METH_VARARGS, nullptr},
    {"presentation_acquire_frame", PresentationAcquireFrame, METH_VARARGS, nullptr},
    {"presentation_clear", PresentationClear, METH_VARARGS, nullptr},
    {"presentation_set_touch_callback", PresentationSetTouch, METH_VARARGS, nullptr},
    {"presentation_set_visibility_callback", PresentationSetVisibility, METH_VARARGS, nullptr},
    {"device_is_ready", DeviceIsReady, METH_VARARGS, nullptr},
    {"device_is_usb_host_online", DeviceUsbOnline, METH_VARARGS, nullptr},
    {"device_is_comm_channel_ready", DeviceCommReady, METH_VARARGS, nullptr},
    {"device_get_comm_channel_id", DeviceChannelId, METH_VARARGS, nullptr},
    {"device_get_runtime_data", DeviceRuntimeData, METH_VARARGS, nullptr},
    {"device_get_rx_info", DeviceRxInfo, METH_VARARGS, nullptr},
    {"device_set_rx_params", DeviceSetRx, METH_VARARGS, nullptr},
    {"device_set_tx_params", DeviceSetTx, METH_VARARGS, nullptr},
    {"device_read_latest_rx_frame", DeviceReadRx, METH_VARARGS, nullptr},
    {"device_create_tx_surface", DeviceCreateTx, METH_VARARGS, nullptr},
    {"device_destroy_tx_surface", DeviceDestroyTx, METH_VARARGS, nullptr},
    {"device_send_keyboard", DeviceSendKeyboard, METH_VARARGS, nullptr},
    {"device_send_mouse", DeviceSendMouse, METH_VARARGS, nullptr},
    {"device_send_comm_data", DeviceSendComm, METH_VARARGS, nullptr},
    {"device_get_app_config_json", DeviceGetConfig, METH_VARARGS, nullptr},
    {"device_update_app_config_json", DeviceUpdateConfig, METH_VARARGS, nullptr},
    {"device_reset_app_config", DeviceResetConfig, METH_VARARGS, nullptr},
    {"device_subscribe_h264", DeviceSubscribeH264, METH_VARARGS, nullptr},
    {"device_unsubscribe_h264", DeviceUnsubscribeH264, METH_VARARGS, nullptr},
    {"device_get_latest_h264_packet_seq", DeviceLatestH264, METH_VARARGS, nullptr},
    {"device_read_h264_packet", DeviceReadH264, METH_VARARGS, nullptr},
    {"device_set_comm_data_callback", DeviceSetCommCallback, METH_VARARGS, nullptr},
    {"device_set_tx_surface_callback", DeviceSetTxCallback, METH_VARARGS, nullptr},
    {"device_set_h264_stream_callback", DeviceSetStreamCallback, METH_VARARGS, nullptr},
    {"device_set_h264_invalidated_callback", DeviceSetInvalidCallback, METH_VARARGS, nullptr},
    {"device_set_error_callback", DeviceSetErrorCallback, METH_VARARGS, nullptr},
    {"device_set_config_changed_callback", DeviceSetConfigCallback, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef kModule = {
    PyModuleDef_HEAD_INIT,
    "_native",
    "Kavio Python binding over the stable Kavio C ABI.",
    -1,
    kMethods,
    nullptr, nullptr, nullptr, nullptr
};

}  // namespace

PyMODINIT_FUNC PyInit__native(void)
{
    g_frame_type.tp_name = "kavio._native.FrameLease";
    g_frame_type.tp_basicsize = sizeof(PyFrameLease);
    g_frame_type.tp_dealloc = reinterpret_cast<destructor>(FrameDealloc);
    g_frame_type.tp_flags = Py_TPFLAGS_DEFAULT;
    g_frame_type.tp_methods = kFrameMethods;
    g_frame_type.tp_as_buffer = &kFrameBuffer;
    if (PyType_Ready(&g_frame_type) < 0) return nullptr;

    g_image_frame_type.tp_name = "kavio._native.ImageFrame";
    g_image_frame_type.tp_basicsize = sizeof(PyImageFrame);
    g_image_frame_type.tp_dealloc = reinterpret_cast<destructor>(ImageFrameDealloc);
    g_image_frame_type.tp_flags = Py_TPFLAGS_DEFAULT;
    g_image_frame_type.tp_methods = kImageFrameMethods;
    g_image_frame_type.tp_as_buffer = &kImageFrameBuffer;
    if (PyType_Ready(&g_image_frame_type) < 0) return nullptr;

    g_h264_packet_type.tp_name = "kavio._native.H264Packet";
    g_h264_packet_type.tp_basicsize = sizeof(PyH264Packet);
    g_h264_packet_type.tp_dealloc = reinterpret_cast<destructor>(H264PacketDealloc);
    g_h264_packet_type.tp_flags = Py_TPFLAGS_DEFAULT;
    g_h264_packet_type.tp_methods = kH264PacketMethods;
    g_h264_packet_type.tp_as_buffer = &kH264PacketBuffer;
    if (PyType_Ready(&g_h264_packet_type) < 0) return nullptr;

    g_tx_surface_type.tp_name = "kavio._native.TxSurface";
    g_tx_surface_type.tp_basicsize = sizeof(PyTxSurface);
    g_tx_surface_type.tp_dealloc = reinterpret_cast<destructor>(TxSurfaceDealloc);
    g_tx_surface_type.tp_flags = Py_TPFLAGS_DEFAULT;
    g_tx_surface_type.tp_methods = kTxSurfaceMethods;
    if (PyType_Ready(&g_tx_surface_type) < 0) return nullptr;

    PyObject* module = PyModule_Create(&kModule);
    if (!module) return nullptr;
    if (PyModule_AddIntConstant(module, "ABI_VERSION", static_cast<long>(kavio_get_abi_version())) != 0) {
        Py_DECREF(module);
        return nullptr;
    }
    struct IntConstant { const char* name; long value; };
    const IntConstant constants[] = {
        {"PIXEL_FORMAT_UNKNOWN", KAVIO_PIXEL_FORMAT_UNKNOWN},
        {"PIXEL_FORMAT_UYVY", KAVIO_PIXEL_FORMAT_UYVY},
        {"PIXEL_FORMAT_YUYV", KAVIO_PIXEL_FORMAT_YUYV},
        {"PIXEL_FORMAT_BGRA8888", KAVIO_PIXEL_FORMAT_BGRA8888},
        {"RUNTIME_VIDEO_STATUS_UNKNOWN", KAVIO_RUNTIME_VIDEO_STATUS_UNKNOWN},
        {"RUNTIME_VIDEO_STATUS_NOT_CONNECTED", KAVIO_RUNTIME_VIDEO_STATUS_NOT_CONNECTED},
        {"RUNTIME_VIDEO_STATUS_CONNECTED", KAVIO_RUNTIME_VIDEO_STATUS_CONNECTED},
        {"RUNTIME_VIDEO_STATUS_RUNNING", KAVIO_RUNTIME_VIDEO_STATUS_RUNNING},
        {"RUNTIME_VIDEO_STATUS_STOPPED", KAVIO_RUNTIME_VIDEO_STATUS_STOPPED},
        {"RUNTIME_VIDEO_STATUS_ERROR", KAVIO_RUNTIME_VIDEO_STATUS_ERROR},
        {"INPUT_MODE_BLE", KAVIO_INPUT_MODE_BLE},
        {"INPUT_MODE_RF24", KAVIO_INPUT_MODE_RF24},
        {"INPUT_MODE_USB", KAVIO_INPUT_MODE_USB},
        {"INPUT_CHANNEL_1", KAVIO_INPUT_CHANNEL_1},
        {"INPUT_CHANNEL_2", KAVIO_INPUT_CHANNEL_2},
        {"INPUT_CHANNEL_3", KAVIO_INPUT_CHANNEL_3},
        {"INPUT_CONNECTION_STATUS_SYNCING", KAVIO_INPUT_CONNECTION_STATUS_SYNCING},
        {"INPUT_CONNECTION_STATUS_PAIRING", KAVIO_INPUT_CONNECTION_STATUS_PAIRING},
        {"INPUT_CONNECTION_STATUS_CONNECTED", KAVIO_INPUT_CONNECTION_STATUS_CONNECTED},
        {"INPUT_CONNECTION_STATUS_DISCONNECTED", KAVIO_INPUT_CONNECTION_STATUS_DISCONNECTED},
        {"H264_PACKET_KEY_FRAME", KAVIO_H264_PACKET_KEY_FRAME},
        {"H264_PACKET_IDR", KAVIO_H264_PACKET_IDR},
        {"H264_PACKET_SPS", KAVIO_H264_PACKET_SPS},
        {"H264_PACKET_PPS", KAVIO_H264_PACKET_PPS},
        {"H264_PACKET_HEADER", KAVIO_H264_PACKET_HEADER},
        {"KEY_OPTION_DOWN", KAVIO_KEY_OPTION_DOWN},
        {"KEY_OPTION_UP", KAVIO_KEY_OPTION_UP},
        {"MOUSE_OPTION_MOVE", KAVIO_MOUSE_OPTION_MOVE},
        {"MOUSE_OPTION_LBUTTON_DOWN", KAVIO_MOUSE_OPTION_LBUTTON_DOWN},
        {"MOUSE_OPTION_LBUTTON_UP", KAVIO_MOUSE_OPTION_LBUTTON_UP},
        {"MOUSE_OPTION_RBUTTON_DOWN", KAVIO_MOUSE_OPTION_RBUTTON_DOWN},
        {"MOUSE_OPTION_RBUTTON_UP", KAVIO_MOUSE_OPTION_RBUTTON_UP},
        {"MOUSE_OPTION_MBUTTON_DOWN", KAVIO_MOUSE_OPTION_MBUTTON_DOWN},
        {"MOUSE_OPTION_MBUTTON_UP", KAVIO_MOUSE_OPTION_MBUTTON_UP},
        {"MOUSE_OPTION_WHEEL", KAVIO_MOUSE_OPTION_WHEEL},
    };
    for (const auto& constant : constants) {
        if (PyModule_AddIntConstant(module, constant.name, constant.value) != 0) {
            Py_DECREF(module);
            return nullptr;
        }
    }
    PyObject* key_codes = PyDict_New();
    if (!key_codes) {
        Py_DECREF(module);
        return nullptr;
    }
    for (const auto& key : kavio::detail::kKeyCodeDescriptors) {
        PyObject* value = PyLong_FromLong(static_cast<long>(kavio::KeyCodeValue(key.code)));
        if (!value || PyDict_SetItemString(key_codes, key.name, value) != 0) {
            Py_XDECREF(value);
            Py_DECREF(key_codes);
            Py_DECREF(module);
            return nullptr;
        }
        Py_DECREF(value);
    }
    if (PyModule_AddObject(module, "KEY_CODES", key_codes) != 0) {
        Py_DECREF(key_codes);
        Py_DECREF(module);
        return nullptr;
    }
    g_error = PyErr_NewException("kavio.Error", PyExc_RuntimeError, nullptr);
    if (!g_error) { Py_DECREF(module); return nullptr; }
    Py_INCREF(g_error);
    if (PyModule_AddObject(module, "Error", g_error) != 0) { Py_DECREF(g_error); Py_DECREF(module); return nullptr; }
    Py_INCREF(&g_frame_type);
    PyModule_AddObject(module, "FrameLease", reinterpret_cast<PyObject*>(&g_frame_type));
    Py_INCREF(&g_image_frame_type);
    PyModule_AddObject(module, "ImageFrame", reinterpret_cast<PyObject*>(&g_image_frame_type));
    Py_INCREF(&g_h264_packet_type);
    PyModule_AddObject(module, "H264Packet", reinterpret_cast<PyObject*>(&g_h264_packet_type));
    Py_INCREF(&g_tx_surface_type);
    PyModule_AddObject(module, "TxSurface", reinterpret_cast<PyObject*>(&g_tx_surface_type));
    return module;
}
