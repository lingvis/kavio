#include <chrono>
#include "KavioLuaInternal.h"

extern "C" {
#include <lualib.h>
}

#include <kavio_c/device.h>
#include <kavio_c/framebuffer.h>
#include <kavio_c/presentation.h>
#include <kavio/Keyboard.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace kl = kavio::lua_binding;

namespace {

struct FrameState {
    KavioFrameLease* frame = nullptr;
    kl::AppState* owner = nullptr;
    uint32_t exports = 0;
    bool active = true;
};

struct BufferState {
    const uint8_t* data = nullptr;
    uint8_t* writable_data = nullptr;
    uint64_t size = 0;
    bool writable = false;
    bool released = false;
    int owner_ref = LUA_NOREF;
    FrameState* frame_owner = nullptr;
};

struct DeviceState {
    KavioDevice* device = nullptr;
    kl::AppState* owner = nullptr;
    int comm_ref = LUA_NOREF;
    int tx_ref = LUA_NOREF;
    int h264_ref = LUA_NOREF;
    int h264_invalidated_ref = LUA_NOREF;
    int error_ref = LUA_NOREF;
    int config_ref = LUA_NOREF;
};

using ImageFrameState = kl::ImageFrameState;

struct TxSurfaceState {
    KavioTxSurface* surface = nullptr;
    kl::AppState* owner = nullptr;
};

struct H264PacketState {
    KavioH264Packet* packet = nullptr;
    kl::AppState* owner = nullptr;
};

template <typename T>
T* CheckState(lua_State* L, int index, const char* metatable)
{
    auto** holder = static_cast<T**>(luaL_checkudata(L, index, metatable));
    if (!holder || !*holder) luaL_error(L, "Kavio object is closed");
    return *holder;
}

template <typename T>
T** NewHolder(lua_State* L, T* value, const char* metatable)
{
    auto** holder = static_cast<T**>(lua_newuserdatauv(L, sizeof(T*), 0));
    *holder = value;
    luaL_getmetatable(L, metatable);
    lua_setmetatable(L, -2);
    return holder;
}

int RaiseStatus(lua_State* L, KavioStatusCode status, const char* scope = nullptr)
{
    if (status == KAVIO_STATUS_OK) return 0;
    const char* message = kavio_last_error_message();
    if (!message || !*message) message = "Kavio operation failed";
    if (scope && *scope) return luaL_error(L, "%s error %d: %s", scope, static_cast<int>(status), message);
    return luaL_error(L, "Kavio error %d: %s", static_cast<int>(status), message);
}

void PushRect(lua_State* L, const KavioRect& rect)
{
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, rect.pos_x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, rect.pos_y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, rect.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, rect.height); lua_setfield(L, -2, "height");
}

void PushFrameInfo(lua_State* L, const KavioFrameInfo& info)
{
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, info.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, info.stride); lua_setfield(L, -2, "stride");
    lua_pushinteger(L, info.pixel_format); lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, static_cast<lua_Integer>(info.buffer_size)); lua_setfield(L, -2, "buffer_size");
    lua_pushinteger(L, info.surface_id); lua_setfield(L, -2, "surface_id");
    lua_pushinteger(L, static_cast<lua_Integer>(info.generation)); lua_setfield(L, -2, "generation");
}

void PushRuntimeVideo(lua_State* L, const KavioRuntimeVideoState& value)
{
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, value.status); lua_setfield(L, -2, "status");
    lua_pushinteger(L, value.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, value.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, value.fps); lua_setfield(L, -2, "fps");
    lua_pushinteger(L, value.pixel_format); lua_setfield(L, -2, "pixel_format");
}

void PushRuntimeData(lua_State* L, const KavioRuntimeData& data)
{
    lua_createtable(L, 0, 3);
    PushRuntimeVideo(L, data.rx); lua_setfield(L, -2, "rx");
    PushRuntimeVideo(L, data.tx); lua_setfield(L, -2, "tx");
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, data.input.mode); lua_setfield(L, -2, "mode");
    lua_pushinteger(L, data.input.channel); lua_setfield(L, -2, "channel");
    lua_pushinteger(L, data.input.connection_status); lua_setfield(L, -2, "connection_status");
    lua_setfield(L, -2, "input");
}

void PushRxInfo(lua_State* L, const KavioRxInfo& info)
{
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, info.connected); lua_setfield(L, -2, "connected");
    lua_pushboolean(L, info.frame_available); lua_setfield(L, -2, "frame_available");
    lua_pushinteger(L, info.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, info.stride); lua_setfield(L, -2, "stride");
    lua_pushinteger(L, info.pixel_format); lua_setfield(L, -2, "pixel_format");
}

void PushImageInfo(lua_State* L, const KavioImageFrameInfo& info)
{
    lua_createtable(L, 0, 11);
    lua_pushinteger(L, static_cast<lua_Integer>(info.frame_id)); lua_setfield(L, -2, "frame_id");
    lua_pushinteger(L, static_cast<lua_Integer>(info.timestamp_ns)); lua_setfield(L, -2, "timestamp_ns");
    lua_pushinteger(L, info.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, info.stride); lua_setfield(L, -2, "stride");
    lua_pushinteger(L, info.pixel_format); lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, info.dest_pos_x); lua_setfield(L, -2, "dest_pos_x");
    lua_pushinteger(L, info.dest_pos_y); lua_setfield(L, -2, "dest_pos_y");
    PushRect(L, info.valid_rect); lua_setfield(L, -2, "valid_rect");
    lua_pushinteger(L, static_cast<lua_Integer>(info.data_size)); lua_setfield(L, -2, "data_size");
}

void PushTxInfo(lua_State* L, const KavioTxSurfaceInfo& info)
{
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, info.surface_id); lua_setfield(L, -2, "surface_id");
    lua_pushinteger(L, info.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, info.stride); lua_setfield(L, -2, "stride");
    lua_pushinteger(L, info.pixel_format); lua_setfield(L, -2, "pixel_format");
}

void PushH264StreamInfo(lua_State* L, const KavioH264StreamInfo& info)
{
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, info.stream_id); lua_setfield(L, -2, "stream_id");
    lua_pushinteger(L, info.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, info.fps); lua_setfield(L, -2, "fps");
    lua_pushinteger(L, info.bitrate_kbps); lua_setfield(L, -2, "bitrate_kbps");
    lua_pushinteger(L, info.gop); lua_setfield(L, -2, "gop");
    lua_pushinteger(L, info.packet_capacity); lua_setfield(L, -2, "packet_capacity");
    lua_pushinteger(L, info.payload_capacity); lua_setfield(L, -2, "payload_capacity");
}

void PushH264PacketInfo(lua_State* L, const KavioH264PacketInfo& info)
{
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, static_cast<lua_Integer>(info.seq)); lua_setfield(L, -2, "seq");
    lua_pushinteger(L, static_cast<lua_Integer>(info.pts_us)); lua_setfield(L, -2, "pts_us");
    lua_pushinteger(L, static_cast<lua_Integer>(info.dts_us)); lua_setfield(L, -2, "dts_us");
    lua_pushinteger(L, static_cast<lua_Integer>(info.timestamp_ns)); lua_setfield(L, -2, "timestamp_ns");
    lua_pushinteger(L, info.flags); lua_setfield(L, -2, "flags");
    lua_pushinteger(L, info.nal_type); lua_setfield(L, -2, "nal_type");
    lua_pushinteger(L, static_cast<lua_Integer>(info.data_size)); lua_setfield(L, -2, "data_size");
}

std::string PopLuaError(lua_State* L)
{
    const char* message = lua_tostring(L, -1);
    std::string value = message ? message : "Lua callback failed";
    lua_pop(L, 1);
    return value;
}

bool CallRef(lua_State* L, int ref, int nargs = 0)
{
    if (ref == LUA_NOREF) {
        if (nargs > 0) lua_pop(L, nargs);
        return true;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (nargs > 0) lua_insert(L, -(nargs + 1));
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) return false;
    return true;
}

void AppCallback(kl::AppState* state, int ref)
{
    if (!state || ref == LUA_NOREF || !state->pending_error.empty()) return;
    if (!CallRef(state->lua, ref)) {
        kl::CaptureAppError(state, PopLuaError(state->lua).c_str());
    }
}

void AppReadyBridge(KavioApp*, void* user_data) { auto* s = static_cast<kl::AppState*>(user_data); AppCallback(s, s ? s->ready_ref : LUA_NOREF); }
void AppTickBridge(KavioApp*, void* user_data) { auto* s = static_cast<kl::AppState*>(user_data); AppCallback(s, s ? s->tick_ref : LUA_NOREF); }
void AppStoppingBridge(KavioApp*, void* user_data) { auto* s = static_cast<kl::AppState*>(user_data); AppCallback(s, s ? s->stopping_ref : LUA_NOREF); }

void TouchBridge(KavioPresentation*, const KavioTouchFrame* frame, void* user_data)
{
    auto* state = static_cast<kl::PresentationState*>(user_data);
    if (!state || state->touch_ref == LUA_NOREF || !frame || !state->owner->pending_error.empty()) return;
    lua_State* L = state->owner->lua;
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, frame->surface_id); lua_setfield(L, -2, "surface_id");
    lua_pushinteger(L, static_cast<lua_Integer>(frame->sequence)); lua_setfield(L, -2, "sequence");
    lua_pushboolean(L, frame->cancelled); lua_setfield(L, -2, "cancelled");
    lua_createtable(L, static_cast<int>(frame->point_count), 0);
    for (uint32_t i = 0; i < frame->point_count; ++i) {
        const auto& point = frame->points[i];
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, point.slot); lua_setfield(L, -2, "slot");
        lua_pushinteger(L, point.tracking_id); lua_setfield(L, -2, "tracking_id");
        lua_pushinteger(L, point.x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, point.y); lua_setfield(L, -2, "y");
        lua_pushinteger(L, point.pressure); lua_setfield(L, -2, "pressure");
        lua_pushinteger(L, point.touch_major); lua_setfield(L, -2, "touch_major");
        lua_pushboolean(L, point.active); lua_setfield(L, -2, "active");
        lua_pushboolean(L, point.changed); lua_setfield(L, -2, "changed");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "points");
    if (!CallRef(L, state->touch_ref, 1)) kl::CaptureAppError(state->owner, PopLuaError(L).c_str());
}

void VisibilityBridge(KavioPresentation*, uint8_t visible, void* user_data)
{
    auto* state = static_cast<kl::PresentationState*>(user_data);
    if (!state || state->visibility_ref == LUA_NOREF || !state->owner->pending_error.empty()) return;
    lua_State* L = state->owner->lua;
    lua_pushboolean(L, visible);
    if (!CallRef(L, state->visibility_ref, 1)) kl::CaptureAppError(state->owner, PopLuaError(L).c_str());
}

int PushBuffer(lua_State* L, const uint8_t* data, uint8_t* writable, uint64_t size, int owner_index, FrameState* frame_owner)
{
    auto* state = new (std::nothrow) BufferState();
    if (!state) return luaL_error(L, "memory allocation failed");
    state->data = data;
    state->writable_data = writable;
    state->size = size;
    state->writable = writable != nullptr;
    state->frame_owner = frame_owner;
    lua_pushvalue(L, owner_index);
    state->owner_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (frame_owner) ++frame_owner->exports;
    NewHolder(L, state, kl::kBufferMetatable);
    return 1;
}

void ReleaseBuffer(lua_State* L, BufferState* state)
{
    if (!state || state->released) return;
    state->released = true;
    if (state->frame_owner && state->frame_owner->exports > 0) --state->frame_owner->exports;
    if (state->owner_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, state->owner_ref);
        state->owner_ref = LUA_NOREF;
    }
    state->data = nullptr;
    state->writable_data = nullptr;
    state->size = 0;
}

int BufferGc(lua_State* L)
{
    auto** holder = static_cast<BufferState**>(luaL_checkudata(L, 1, kl::kBufferMetatable));
    if (holder && *holder) {
        ReleaseBuffer(L, *holder);
        delete *holder;
        *holder = nullptr;
    }
    return 0;
}

int BufferRelease(lua_State* L)
{
    auto* state = CheckState<BufferState>(L, 1, kl::kBufferMetatable);
    ReleaseBuffer(L, state);
    return 0;
}

int BufferLen(lua_State* L)
{
    auto* state = CheckState<BufferState>(L, 1, kl::kBufferMetatable);
    lua_pushinteger(L, static_cast<lua_Integer>(state->released ? 0 : state->size));
    return 1;
}

int BufferAddress(lua_State* L)
{
    auto* state = CheckState<BufferState>(L, 1, kl::kBufferMetatable);
    if (state->released) return luaL_error(L, "buffer view is released");
    lua_pushlightuserdata(L, const_cast<uint8_t*>(state->data));
    lua_pushinteger(L, static_cast<lua_Integer>(state->size));
    return 2;
}

int BufferRead(lua_State* L)
{
    auto* state = CheckState<BufferState>(L, 1, kl::kBufferMetatable);
    if (state->released) return luaL_error(L, "buffer view is released");
    lua_Integer offset = luaL_optinteger(L, 2, 0);
    lua_Integer length = luaL_optinteger(L, 3, static_cast<lua_Integer>(state->size) - offset);
    if (offset < 0 || length < 0 || static_cast<uint64_t>(offset) > state->size ||
        static_cast<uint64_t>(length) > state->size - static_cast<uint64_t>(offset)) {
        return luaL_error(L, "buffer range is out of bounds");
    }
    if (length == 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(state->data + offset), static_cast<size_t>(length));
    return 1;
}

int BufferWrite(lua_State* L)
{
    auto* state = CheckState<BufferState>(L, 1, kl::kBufferMetatable);
    if (state->released) return luaL_error(L, "buffer view is released");
    if (!state->writable) return luaL_error(L, "buffer is read-only");
    lua_Integer offset = luaL_optinteger(L, 2, 0);
    size_t size = 0;
    const char* bytes = luaL_checklstring(L, 3, &size);
    if (offset < 0 || static_cast<uint64_t>(offset) > state->size || size > state->size - static_cast<uint64_t>(offset)) {
        return luaL_error(L, "buffer write is out of bounds");
    }
    if (size != 0) std::memcpy(state->writable_data + offset, bytes, size);
    return 0;
}

int AppGc(lua_State* L)
{
    auto** holder = static_cast<kl::AppState**>(luaL_checkudata(L, 1, kl::kAppMetatable));
    if (holder && *holder) {
        if ((*holder)->running) (void)kavio_app_request_stop((*holder)->app);
        kl::ReleaseApp(*holder);
        *holder = nullptr;
    }
    return 0;
}

int AppCreate(lua_State* L)
{
    KavioAppOptions options{};
    kavio_app_options_init(&options);
    std::string client_name;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "client_name");
        if (!lua_isnil(L, -1)) { client_name = luaL_checkstring(L, -1); options.client_name = client_name.c_str(); }
        lua_pop(L, 1);
        lua_getfield(L, 1, "idle_sleep_ms"); if (!lua_isnil(L,-1)) options.idle_sleep_ms = static_cast<uint32_t>(luaL_checkinteger(L,-1)); lua_pop(L,1);
        lua_getfield(L, 1, "install_signal_handlers"); if (!lua_isnil(L,-1)) options.install_signal_handlers = lua_toboolean(L,-1) ? 1u : 0u; lua_pop(L,1);
        lua_getfield(L, 1, "rx_enabled"); if (!lua_isnil(L,-1)) options.rx_enabled = lua_toboolean(L,-1) ? 1u : 0u; lua_pop(L,1);
        lua_getfield(L, 1, "tx_enabled"); if (!lua_isnil(L,-1)) options.tx_enabled = lua_toboolean(L,-1) ? 1u : 0u; lua_pop(L,1);
        lua_getfield(L, 1, "rx_frame_sample_interval"); if (!lua_isnil(L,-1)) options.rx_frame_sample_interval = static_cast<uint32_t>(luaL_checkinteger(L,-1)); lua_pop(L,1);
        lua_getfield(L, 1, "tx_z_order"); if (!lua_isnil(L,-1)) options.tx_z_order = static_cast<int32_t>(luaL_checkinteger(L,-1)); lua_pop(L,1);
    }
    KavioApp* app = nullptr;
    const auto status = kavio_app_create(&options, &app);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    auto* state = new (std::nothrow) kl::AppState();
    if (!state) { (void)kavio_app_destroy(app); return luaL_error(L, "memory allocation failed"); }
    state->lua = L;
    state->app = app;
    NewHolder(L, state, kl::kAppMetatable);
    return 1;
}

int SetAppCallback(lua_State* L, int which)
{
    auto* state = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    if (!lua_isnoneornil(L, 2) && !lua_isfunction(L, 2)) return luaL_error(L, "callback must be a function or nil");
    int* slot = which == 0 ? &state->ready_ref : (which == 1 ? &state->tick_ref : &state->stopping_ref);
    kl::ReplaceLuaRef(L, *slot, 2);
    KavioStatusCode status = KAVIO_STATUS_OK;
    if (which == 0) status = kavio_app_set_ready_callback(state->app, *slot == LUA_NOREF ? nullptr : AppReadyBridge, state);
    else if (which == 1) status = kavio_app_set_tick_callback(state->app, *slot == LUA_NOREF ? nullptr : AppTickBridge, state);
    else status = kavio_app_set_stopping_callback(state->app, *slot == LUA_NOREF ? nullptr : AppStoppingBridge, state);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return 0;
}

int AppReady(lua_State* L) { return SetAppCallback(L, 0); }
int AppTick(lua_State* L) { return SetAppCallback(L, 1); }
int AppStopping(lua_State* L) { return SetAppCallback(L, 2); }

int AppRun(lua_State* L)
{
    auto* state = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    if (state->running) return luaL_error(L, "App is already running");
    state->pending_error.clear();
    state->running = true;
    int32_t exit_code = 0;
    const auto status = kavio_app_run(state->app, &exit_code);
    state->running = false;
    if (!state->pending_error.empty()) {
        std::string error = std::move(state->pending_error);
        state->pending_error.clear();
        return luaL_error(L, "%s", error.c_str());
    }
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    lua_pushinteger(L, exit_code);
    return 1;
}

int AppRequestStop(lua_State* L)
{
    auto* state = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    const auto status = kavio_app_request_stop(state->app);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return 0;
}

int AppLastStatus(lua_State* L)
{
    auto* state = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    lua_pushinteger(L, kavio_app_last_status(state->app));
    return 1;
}

int PresentationGc(lua_State* L)
{
    auto** holder = static_cast<kl::PresentationState**>(luaL_checkudata(L, 1, kl::kPresentationMetatable));
    if (holder && *holder) {
        auto* state = *holder;
        if (state->presentation) {
            (void)kavio_presentation_set_touch_callback(state->presentation, nullptr, nullptr);
            (void)kavio_presentation_set_visibility_callback(state->presentation, nullptr, nullptr);
        }
        if (state->touch_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, state->touch_ref);
        if (state->visibility_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, state->visibility_ref);
        kl::ReleaseApp(state->owner);
        delete state;
        *holder = nullptr;
    }
    return 0;
}

int AppPresentation(lua_State* L)
{
    auto* owner = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    KavioPresentation* presentation = nullptr;
    const auto status = kavio_app_get_presentation(owner->app, &presentation);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    auto* state = new (std::nothrow) kl::PresentationState();
    if (!state) return luaL_error(L, "memory allocation failed");
    state->presentation = presentation;
    state->owner = owner;
    kl::RetainApp(owner);
    NewHolder(L, state, kl::kPresentationMetatable);
    return 1;
}

int PresentationIsReady(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    lua_pushboolean(L, kavio_presentation_is_ready(state->presentation));
    return 1;
}

int PresentationIsVisible(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    lua_pushboolean(L, kavio_presentation_is_visible(state->presentation));
    return 1;
}

int PresentationGeneration(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    lua_pushinteger(L, static_cast<lua_Integer>(kavio_presentation_generation(state->presentation)));
    return 1;
}

int PresentationFrameInfo(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    KavioFrameInfo info{}; kavio_frame_info_init(&info);
    const auto status = kavio_presentation_get_frame_info(state->presentation, &info);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    PushFrameInfo(L, info);
    return 1;
}

int FrameGc(lua_State* L)
{
    auto** holder = static_cast<FrameState**>(luaL_checkudata(L, 1, kl::kFrameMetatable));
    if (holder && *holder) {
        auto* state = *holder;
        if (state->frame) (void)kavio_frame_destroy(state->frame);
        kl::ReleaseApp(state->owner);
        delete state;
        *holder = nullptr;
    }
    return 0;
}

int PresentationAcquire(lua_State* L)
{
    auto* presentation = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    KavioFrameLease* frame = nullptr;
    const auto status = kavio_presentation_acquire_frame(presentation->presentation, &frame);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    auto* state = new (std::nothrow) FrameState();
    if (!state) { (void)kavio_frame_destroy(frame); return luaL_error(L, "memory allocation failed"); }
    state->frame = frame;
    state->owner = presentation->owner;
    kl::RetainApp(state->owner);
    NewHolder(L, state, kl::kFrameMetatable);
    return 1;
}

int PresentationClear(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    const int b = static_cast<int>(luaL_optinteger(L, 2, 0));
    const int g = static_cast<int>(luaL_optinteger(L, 3, 0));
    const int r = static_cast<int>(luaL_optinteger(L, 4, 0));
    const int a = static_cast<int>(luaL_optinteger(L, 5, 255));
    const auto status = kavio_presentation_clear(state->presentation, static_cast<uint8_t>(b), static_cast<uint8_t>(g), static_cast<uint8_t>(r), static_cast<uint8_t>(a));
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return 0;
}

int PresentationSetTouch(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    if (!lua_isnoneornil(L, 2) && !lua_isfunction(L, 2)) return luaL_error(L, "callback must be a function or nil");
    kl::ReplaceLuaRef(L, state->touch_ref, 2);
    const auto status = kavio_presentation_set_touch_callback(state->presentation, state->touch_ref == LUA_NOREF ? nullptr : TouchBridge, state);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return 0;
}

int PresentationSetVisibility(lua_State* L)
{
    auto* state = CheckState<kl::PresentationState>(L, 1, kl::kPresentationMetatable);
    if (!lua_isnoneornil(L, 2) && !lua_isfunction(L, 2)) return luaL_error(L, "callback must be a function or nil");
    kl::ReplaceLuaRef(L, state->visibility_ref, 2);
    const auto status = kavio_presentation_set_visibility_callback(state->presentation, state->visibility_ref == LUA_NOREF ? nullptr : VisibilityBridge, state);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return 0;
}

int FrameInfo(lua_State* L)
{
    auto* state = CheckState<FrameState>(L, 1, kl::kFrameMetatable);
    if (!state->active) return luaL_error(L, "FrameLease is no longer active");
    KavioFrameInfo info{}; kavio_frame_info_init(&info);
    const auto status = kavio_frame_get_info(state->frame, &info);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    PushFrameInfo(L, info);
    return 1;
}

int FrameBuffer(lua_State* L)
{
    auto* state = CheckState<FrameState>(L, 1, kl::kFrameMetatable);
    if (!state->active) return luaL_error(L, "FrameLease is no longer active");
    void* pixels = nullptr; uint64_t size = 0;
    const auto status = kavio_frame_get_pixels(state->frame, &pixels, &size);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    return PushBuffer(L, static_cast<const uint8_t*>(pixels), static_cast<uint8_t*>(pixels), size, 1, state);
}

int FrameWithBuffer(lua_State* L)
{
    auto* frame = CheckState<FrameState>(L, 1, kl::kFrameMetatable);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!frame->active) return luaL_error(L, "FrameLease is no longer active");
    void* pixels = nullptr; uint64_t size = 0;
    const auto status = kavio_frame_get_pixels(frame->frame, &pixels, &size);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    lua_pushvalue(L, 2);
    PushBuffer(L, static_cast<const uint8_t*>(pixels), static_cast<uint8_t*>(pixels), size, 1, frame);
    auto* view = CheckState<BufferState>(L, -1, kl::kBufferMetatable);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::string error = PopLuaError(L);
        ReleaseBuffer(L, view);
        return luaL_error(L, "%s", error.c_str());
    }
    ReleaseBuffer(L, view);
    return 0;
}

std::vector<KavioRect> ReadDirtyRects(lua_State* L, int index)
{
    std::vector<KavioRect> result;
    if (lua_isnoneornil(L, index)) return result;
    luaL_checktype(L, index, LUA_TTABLE);
    const auto count = luaL_len(L, index);
    result.reserve(static_cast<size_t>(count));
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, index, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        KavioRect rect{};
        lua_getfield(L, -1, "x"); rect.pos_x = static_cast<int32_t>(luaL_optinteger(L, -1, 0)); lua_pop(L, 1);
        lua_getfield(L, -1, "y"); rect.pos_y = static_cast<int32_t>(luaL_optinteger(L, -1, 0)); lua_pop(L, 1);
        lua_getfield(L, -1, "width"); rect.width = static_cast<uint32_t>(luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, -1, "height"); rect.height = static_cast<uint32_t>(luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_pop(L, 1);
        result.push_back(rect);
    }
    return result;
}

int FramePresent(lua_State* L)
{
    auto* state = CheckState<FrameState>(L, 1, kl::kFrameMetatable);
    if (!state->active) return luaL_error(L, "FrameLease is no longer active");
    if (state->exports != 0) return luaL_error(L, "release all BufferView objects before Present");
    const auto rects = ReadDirtyRects(L, 2);
    const auto status = kavio_frame_present(state->frame, rects.empty() ? nullptr : rects.data(), static_cast<uint32_t>(rects.size()));
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    state->active = false;
    return 0;
}

int FrameDiscard(lua_State* L)
{
    auto* state = CheckState<FrameState>(L, 1, kl::kFrameMetatable);
    if (!state->active) return 0;
    if (state->exports != 0) return luaL_error(L, "release all BufferView objects before Discard");
    const auto status = kavio_frame_discard(state->frame);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    state->active = false;
    return 0;
}

void DeviceCallbackError(DeviceState* state, const std::string& error)
{
    if (state && state->owner) kl::CaptureAppError(state->owner, error.c_str());
}

bool CallDeviceRef(DeviceState* state, int ref, int nargs)
{
    if (!state || ref == LUA_NOREF || !state->owner || !state->owner->pending_error.empty()) {
        if (state && nargs > 0) lua_pop(state->owner->lua, nargs);
        return true;
    }
    lua_State* L = state->owner->lua;
    if (!CallRef(L, ref, nargs)) {
        DeviceCallbackError(state, PopLuaError(L));
        return false;
    }
    return true;
}

void CommBridge(KavioDevice*, const uint8_t* data, uint64_t size, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->comm_ref == LUA_NOREF) return;
    lua_pushlstring(state->owner->lua, data ? reinterpret_cast<const char*>(data) : "", static_cast<size_t>(size));
    (void)CallDeviceRef(state, state->comm_ref, 1);
}

bool PushTxSurfaceCallback(lua_State* L, KavioTxSurface* surface, kl::AppState* owner)
{
    auto* state = new (std::nothrow) TxSurfaceState{surface, owner};
    if (!state) {
        kavio_tx_surface_destroy(surface);
        return false;
    }
    kl::RetainApp(owner);
    NewHolder(L, state, kl::kTxSurfaceMetatable);
    return true;
}

void TxBridge(KavioDevice*, uint64_t request_id, KavioStatusCode status, KavioTxSurface* surface, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->tx_ref == LUA_NOREF) { if (surface) kavio_tx_surface_destroy(surface); return; }
    lua_State* L = state->owner->lua;
    lua_pushinteger(L, static_cast<lua_Integer>(request_id));
    lua_pushinteger(L, status);
    if (surface) {
        if (!PushTxSurfaceCallback(L, surface, state->owner)) {
            kl::CaptureAppError(state->owner, "memory allocation failed while creating TX Surface Lua object");
            lua_pop(L, 2);
            return;
        }
    } else {
        lua_pushnil(L);
    }
    (void)CallDeviceRef(state, state->tx_ref, 3);
}

void H264Bridge(KavioDevice*, uint64_t request_id, KavioStatusCode status, const KavioH264StreamInfo* info, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->h264_ref == LUA_NOREF) return;
    lua_State* L = state->owner->lua;
    lua_pushinteger(L, static_cast<lua_Integer>(request_id));
    lua_pushinteger(L, status);
    if (info) PushH264StreamInfo(L, *info); else lua_pushnil(L);
    (void)CallDeviceRef(state, state->h264_ref, 3);
}

void H264InvalidatedBridge(KavioDevice*, const KavioH264InvalidatedEvent* event, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->h264_invalidated_ref == LUA_NOREF || !event) return;
    lua_State* L = state->owner->lua;
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, event->stream_id); lua_setfield(L, -2, "stream_id");
    lua_pushinteger(L, event->reason); lua_setfield(L, -2, "reason");
    (void)CallDeviceRef(state, state->h264_invalidated_ref, 1);
}

void ErrorBridge(KavioDevice*, const KavioDeviceErrorEvent* event, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->error_ref == LUA_NOREF || !event) return;
    lua_State* L = state->owner->lua;
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, static_cast<lua_Integer>(event->track_id)); lua_setfield(L, -2, "track_id");
    lua_pushinteger(L, event->code); lua_setfield(L, -2, "code");
    lua_pushstring(L, event->message ? event->message : ""); lua_setfield(L, -2, "message");
    (void)CallDeviceRef(state, state->error_ref, 1);
}

void ConfigBridge(KavioDevice*, void* user_data)
{
    auto* state = static_cast<DeviceState*>(user_data);
    if (!state || state->config_ref == LUA_NOREF) return;
    (void)CallDeviceRef(state, state->config_ref, 0);
}

int DeviceGc(lua_State* L)
{
    auto** holder = static_cast<DeviceState**>(luaL_checkudata(L, 1, kl::kDeviceMetatable));
    if (holder && *holder) {
        auto* state = *holder;
        if (state->device) {
            (void)kavio_device_set_comm_data_callback(state->device, nullptr, nullptr);
            (void)kavio_device_set_tx_surface_callback(state->device, nullptr, nullptr);
            (void)kavio_device_set_h264_stream_callback(state->device, nullptr, nullptr);
            (void)kavio_device_set_h264_invalidated_callback(state->device, nullptr, nullptr);
            (void)kavio_device_set_error_callback(state->device, nullptr, nullptr);
            (void)kavio_device_set_config_changed_callback(state->device, nullptr, nullptr);
        }
        for (int* ref : {&state->comm_ref, &state->tx_ref, &state->h264_ref, &state->h264_invalidated_ref, &state->error_ref, &state->config_ref}) {
            if (*ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, *ref);
        }
        kl::ReleaseApp(state->owner);
        delete state;
        *holder = nullptr;
    }
    return 0;
}

int AppDevice(lua_State* L)
{
    auto* owner = CheckState<kl::AppState>(L, 1, kl::kAppMetatable);
    KavioDevice* device = nullptr;
    const auto status = kavio_app_get_device(owner->app, &device);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    auto* state = new (std::nothrow) DeviceState();
    if (!state) return luaL_error(L, "memory allocation failed");
    state->device = device;
    state->owner = owner;
    kl::RetainApp(owner);
    NewHolder(L, state, kl::kDeviceMetatable);
    return 1;
}

int DeviceIsReady(lua_State* L) { auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); lua_pushboolean(L,kavio_device_is_ready(s->device)); return 1; }
int DeviceUsbOnline(lua_State* L) { auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); lua_pushboolean(L,kavio_device_is_usb_host_online(s->device)); return 1; }
int DeviceCommReady(lua_State* L) { auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); lua_pushboolean(L,kavio_device_is_comm_channel_ready(s->device)); return 1; }
int DeviceCommId(lua_State* L) { auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); lua_pushinteger(L,kavio_device_get_comm_channel_id(s->device)); return 1; }

int DeviceRuntimeData(lua_State* L)
{
    auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); KavioRuntimeData d{}; kavio_runtime_data_init(&d);
    const auto status=kavio_device_get_runtime_data(s->device,&d); if(status!=KAVIO_STATUS_OK)return RaiseStatus(L,status); PushRuntimeData(L,d); return 1;
}

int DeviceRxInfo(lua_State* L)
{
    auto* s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); KavioRxInfo d{}; kavio_rx_info_init(&d);
    const auto status=kavio_device_get_rx_info(s->device,&d); if(status!=KAVIO_STATUS_OK)return RaiseStatus(L,status); PushRxInfo(L,d); return 1;
}

int DeviceSetRx(lua_State* L)
{
    auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); uint64_t id=0;
    const auto status=kavio_device_set_rx_params(s->device,lua_toboolean(L,2)?1u:0u,static_cast<uint32_t>(luaL_optinteger(L,3,1)),&id);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int DeviceSetTx(lua_State* L)
{
    auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable); uint64_t id=0;
    const auto status=kavio_device_set_tx_params(s->device,lua_toboolean(L,2)?1u:0u,static_cast<int32_t>(luaL_optinteger(L,3,0)),&id);
    if (status != KAVIO_STATUS_OK) return RaiseStatus(L, status);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int ImageGc(lua_State* L)
{
    auto**h=static_cast<ImageFrameState**>(luaL_checkudata(L,1,kl::kImageFrameMetatable));if(h&&*h){kavio_image_frame_destroy((*h)->frame);kl::ReleaseApp((*h)->owner);delete*h;*h=nullptr;}return 0;
}

int DeviceReadRx(lua_State* L)
{
    auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);KavioImageFrame*f=nullptr;const auto status=kavio_device_read_latest_rx_frame(s->device,&f);if(status!=KAVIO_STATUS_OK)return RaiseStatus(L,status);
    auto*state=new(std::nothrow)ImageFrameState{f,s->owner};if(!state){kavio_image_frame_destroy(f);return luaL_error(L,"memory allocation failed");}kl::RetainApp(s->owner);NewHolder(L,state,kl::kImageFrameMetatable);return 1;
}

int ImageInfo(lua_State* L){auto*s=CheckState<ImageFrameState>(L,1,kl::kImageFrameMetatable);KavioImageFrameInfo i{};kavio_image_frame_info_init(&i);auto st=kavio_image_frame_get_info(s->frame,&i);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);PushImageInfo(L,i);return 1;}
int ImageBuffer(lua_State* L){auto*s=CheckState<ImageFrameState>(L,1,kl::kImageFrameMetatable);const uint8_t*d=nullptr;uint64_t n=0;auto st=kavio_image_frame_get_data(s->frame,&d,&n);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);return PushBuffer(L,d,nullptr,n,1,nullptr);}

int TxGc(lua_State* L){auto**h=static_cast<TxSurfaceState**>(luaL_checkudata(L,1,kl::kTxSurfaceMetatable));if(h&&*h){kavio_tx_surface_destroy((*h)->surface);kl::ReleaseApp((*h)->owner);delete*h;*h=nullptr;}return 0;}
int TxInfo(lua_State* L){auto*s=CheckState<TxSurfaceState>(L,1,kl::kTxSurfaceMetatable);KavioTxSurfaceInfo i{};kavio_tx_surface_info_init(&i);auto st=kavio_tx_surface_get_info(s->surface,&i);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);PushTxInfo(L,i);return 1;}
int TxClear(lua_State* L){auto*s=CheckState<TxSurfaceState>(L,1,kl::kTxSurfaceMetatable);auto st=kavio_tx_surface_clear(s->surface);if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}
int TxRequestDestroy(lua_State* L){auto*s=CheckState<TxSurfaceState>(L,1,kl::kTxSurfaceMetatable);uint64_t id=0;auto st=kavio_tx_surface_request_destroy(s->surface,&id);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushinteger(L,static_cast<lua_Integer>(id));return 1;}

bool ReadRectOptional(lua_State* L,int index,KavioRect& rect)
{
    if (lua_isnoneornil(L, index)) return false;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L,index,"x");rect.pos_x=static_cast<int32_t>(luaL_optinteger(L,-1,0));lua_pop(L,1);
    lua_getfield(L,index,"y");rect.pos_y=static_cast<int32_t>(luaL_optinteger(L,-1,0));lua_pop(L,1);
    lua_getfield(L,index,"width");rect.width=static_cast<uint32_t>(luaL_checkinteger(L,-1));lua_pop(L,1);
    lua_getfield(L,index,"height");rect.height=static_cast<uint32_t>(luaL_checkinteger(L,-1));lua_pop(L,1);return true;
}

int TxWrite(lua_State* L)
{
    auto*s=CheckState<TxSurfaceState>(L,1,kl::kTxSurfaceMetatable);size_t n=0;const char*data=luaL_checklstring(L,2,&n);
    const uint32_t w=static_cast<uint32_t>(luaL_checkinteger(L,3));const uint32_t h=static_cast<uint32_t>(luaL_checkinteger(L,4));const uint32_t stride=static_cast<uint32_t>(luaL_checkinteger(L,5));const uint32_t fmt=static_cast<uint32_t>(luaL_checkinteger(L,6));
    const int32_t x=static_cast<int32_t>(luaL_optinteger(L,7,0));const int32_t y=static_cast<int32_t>(luaL_optinteger(L,8,0));KavioRect rect{};const bool has=ReadRectOptional(L,9,rect);
    auto st=kavio_tx_surface_write(s->surface,data,n,w,h,stride,fmt,x,y,has?&rect:nullptr);if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;
}

int DeviceCreateTx(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);uint64_t id=0;auto st=kavio_device_create_tx_surface(s->device,static_cast<uint32_t>(luaL_checkinteger(L,2)),static_cast<uint32_t>(luaL_checkinteger(L,3)),&id);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushinteger(L,static_cast<lua_Integer>(id));return 1;}
int DeviceDestroyTx(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);uint64_t id=0;auto st=kavio_device_destroy_tx_surface(s->device,static_cast<uint32_t>(luaL_checkinteger(L,2)),&id);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushinteger(L,static_cast<lua_Integer>(id));return 1;}
int DeviceKeyboard(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);auto st=kavio_device_send_keyboard(s->device,static_cast<int32_t>(luaL_checkinteger(L,2)),static_cast<int32_t>(luaL_checkinteger(L,3)),static_cast<uint64_t>(luaL_optinteger(L,4,0)));if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}
int DeviceMouse(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);auto st=kavio_device_send_mouse(s->device,static_cast<int32_t>(luaL_checkinteger(L,2)),static_cast<int32_t>(luaL_optinteger(L,3,0)),static_cast<int32_t>(luaL_optinteger(L,4,0)),static_cast<int32_t>(luaL_optinteger(L,5,0)),static_cast<uint64_t>(luaL_optinteger(L,6,0)));if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}
int DeviceComm(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);size_t n=0;const char*d=luaL_checklstring(L,2,&n);auto st=kavio_device_send_comm_data(s->device,d,n);if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}

int DeviceGetConfig(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);char*json=nullptr;uint64_t n=0;auto st=kavio_device_get_app_config_json(s->device,&json,&n);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushlstring(L,json?json:"",static_cast<size_t>(n));kavio_free(json);return 1;}
int DeviceUpdateConfig(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);size_t n=0;const char*json=luaL_checklstring(L,2,&n);auto st=kavio_device_update_app_config_json(s->device,json,n);if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}
int DeviceResetConfig(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);auto st=kavio_device_reset_app_config(s->device);if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;}
int DeviceSubscribeH264(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);uint64_t id=0;auto st=kavio_device_subscribe_h264(s->device,&id);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushinteger(L,static_cast<lua_Integer>(id));return 1;}
int DeviceUnsubscribeH264(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);uint64_t id=0;auto st=kavio_device_unsubscribe_h264(s->device,static_cast<uint32_t>(luaL_checkinteger(L,2)),&id);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);lua_pushinteger(L,static_cast<lua_Integer>(id));return 1;}
int DeviceLatestH264(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);lua_pushinteger(L,static_cast<lua_Integer>(kavio_device_get_latest_h264_packet_seq(s->device,static_cast<uint32_t>(luaL_checkinteger(L,2)))));return 1;}

int H264Gc(lua_State* L){auto**h=static_cast<H264PacketState**>(luaL_checkudata(L,1,kl::kH264PacketMetatable));if(h&&*h){kavio_h264_packet_destroy((*h)->packet);kl::ReleaseApp((*h)->owner);delete*h;*h=nullptr;}return 0;}
int DeviceReadH264(lua_State* L){auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);KavioH264Packet*p=nullptr;auto st=kavio_device_read_h264_packet(s->device,static_cast<uint32_t>(luaL_checkinteger(L,2)),static_cast<uint64_t>(luaL_checkinteger(L,3)),&p);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);auto*state=new(std::nothrow)H264PacketState{p,s->owner};if(!state){kavio_h264_packet_destroy(p);return luaL_error(L,"memory allocation failed");}kl::RetainApp(s->owner);NewHolder(L,state,kl::kH264PacketMetatable);return 1;}
int H264Info(lua_State* L){auto*s=CheckState<H264PacketState>(L,1,kl::kH264PacketMetatable);KavioH264PacketInfo i{};kavio_h264_packet_info_init(&i);auto st=kavio_h264_packet_get_info(s->packet,&i);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);PushH264PacketInfo(L,i);return 1;}
int H264Buffer(lua_State* L){auto*s=CheckState<H264PacketState>(L,1,kl::kH264PacketMetatable);const uint8_t*d=nullptr;uint64_t n=0;auto st=kavio_h264_packet_get_data(s->packet,&d,&n);if(st!=KAVIO_STATUS_OK)return RaiseStatus(L,st);return PushBuffer(L,d,nullptr,n,1,nullptr);}

int SetDeviceCallback(lua_State* L, int which)
{
    auto*s=CheckState<DeviceState>(L,1,kl::kDeviceMetatable);if(!lua_isnoneornil(L,2)&&!lua_isfunction(L,2))return luaL_error(L,"callback must be a function or nil");
    int*slot=nullptr;KavioStatusCode st=KAVIO_STATUS_OK;
    switch(which){
    case 0:slot=&s->comm_ref;break;case 1:slot=&s->tx_ref;break;case 2:slot=&s->h264_ref;break;case 3:slot=&s->h264_invalidated_ref;break;case 4:slot=&s->error_ref;break;default:slot=&s->config_ref;break;}
    kl::ReplaceLuaRef(L,*slot,2);
    switch(which){
    case 0:st=kavio_device_set_comm_data_callback(s->device,*slot==LUA_NOREF?nullptr:CommBridge,s);break;
    case 1:st=kavio_device_set_tx_surface_callback(s->device,*slot==LUA_NOREF?nullptr:TxBridge,s);break;
    case 2:st=kavio_device_set_h264_stream_callback(s->device,*slot==LUA_NOREF?nullptr:H264Bridge,s);break;
    case 3:st=kavio_device_set_h264_invalidated_callback(s->device,*slot==LUA_NOREF?nullptr:H264InvalidatedBridge,s);break;
    case 4:st=kavio_device_set_error_callback(s->device,*slot==LUA_NOREF?nullptr:ErrorBridge,s);break;
    default:st=kavio_device_set_config_changed_callback(s->device,*slot==LUA_NOREF?nullptr:ConfigBridge,s);break;}
    if (st != KAVIO_STATUS_OK) return RaiseStatus(L, st);
    return 0;
}
int DeviceOnComm(lua_State*L){return SetDeviceCallback(L,0);}int DeviceOnTx(lua_State*L){return SetDeviceCallback(L,1);}int DeviceOnH264(lua_State*L){return SetDeviceCallback(L,2);}int DeviceOnH264Invalid(lua_State*L){return SetDeviceCallback(L,3);}int DeviceOnError(lua_State*L){return SetDeviceCallback(L,4);}int DeviceOnConfig(lua_State*L){return SetDeviceCallback(L,5);}

void CreateMetatable(lua_State* L, const char* name, const luaL_Reg* methods, lua_CFunction gc = nullptr, lua_CFunction len = nullptr)
{
    luaL_newmetatable(L, name);
    kl::SetMethods(L, methods);
    if (gc) { lua_pushcfunction(L, gc); lua_setfield(L, -2, "__gc"); }
    if (len) { lua_pushcfunction(L, len); lua_setfield(L, -2, "__len"); }
    lua_pop(L, 1);
}

const luaL_Reg kAppMethods[]={{"run",AppRun},{"request_stop",AppRequestStop},{"last_status",AppLastStatus},{"presentation",AppPresentation},{"device",AppDevice},{"set_ready_callback",AppReady},{"set_tick_callback",AppTick},{"set_stopping_callback",AppStopping},{nullptr,nullptr}};
const luaL_Reg kPresentationMethods[]={{"is_ready",PresentationIsReady},{"is_visible",PresentationIsVisible},{"generation",PresentationGeneration},{"frame_info",PresentationFrameInfo},{"acquire_frame",PresentationAcquire},{"clear",PresentationClear},{"set_touch_callback",PresentationSetTouch},{"set_visibility_callback",PresentationSetVisibility},{nullptr,nullptr}};
const luaL_Reg kFrameMethods[]={{"info",FrameInfo},{"buffer",FrameBuffer},{"with_buffer",FrameWithBuffer},{"present",FramePresent},{"discard",FrameDiscard},{nullptr,nullptr}};
const luaL_Reg kBufferMethods[]={{"release",BufferRelease},{"address",BufferAddress},{"read",BufferRead},{"write",BufferWrite},{nullptr,nullptr}};
const luaL_Reg kDeviceMethods[]={{"is_ready",DeviceIsReady},{"is_usb_host_online",DeviceUsbOnline},{"is_comm_channel_ready",DeviceCommReady},{"comm_channel_id",DeviceCommId},{"runtime_data",DeviceRuntimeData},{"rx_info",DeviceRxInfo},{"set_rx_params",DeviceSetRx},{"set_tx_params",DeviceSetTx},{"read_latest_rx_frame",DeviceReadRx},{"create_tx_surface",DeviceCreateTx},{"destroy_tx_surface",DeviceDestroyTx},{"send_keyboard",DeviceKeyboard},{"send_mouse",DeviceMouse},{"send_comm_data",DeviceComm},{"get_app_config_json",DeviceGetConfig},{"update_app_config_json",DeviceUpdateConfig},{"reset_app_config",DeviceResetConfig},{"subscribe_h264",DeviceSubscribeH264},{"unsubscribe_h264",DeviceUnsubscribeH264},{"latest_h264_packet_seq",DeviceLatestH264},{"read_h264_packet",DeviceReadH264},{"set_comm_data_callback",DeviceOnComm},{"set_tx_surface_callback",DeviceOnTx},{"set_h264_stream_callback",DeviceOnH264},{"set_h264_invalidated_callback",DeviceOnH264Invalid},{"set_error_callback",DeviceOnError},{"set_config_changed_callback",DeviceOnConfig},{nullptr,nullptr}};
const luaL_Reg kImageMethods[]={{"info",ImageInfo},{"buffer",ImageBuffer},{nullptr,nullptr}};
const luaL_Reg kTxMethods[]={{"info",TxInfo},{"write",TxWrite},{"clear",TxClear},{"request_destroy",TxRequestDestroy},{nullptr,nullptr}};
const luaL_Reg kH264Methods[]={{"info",H264Info},{"buffer",H264Buffer},{nullptr,nullptr}};

}  // namespace

namespace kavio::lua_binding {

void CaptureAppError(AppState* state, const char* message)
{
    if (!state || !state->pending_error.empty()) return;
    state->pending_error = message && *message ? message : "Lua callback failed";
    if (state->app) (void)kavio_app_request_stop(state->app);
}

void ReleaseApp(AppState* state)
{
    if (!state || --state->references != 0) return;
    if (state->app) {
        (void)kavio_app_set_ready_callback(state->app, nullptr, nullptr);
        (void)kavio_app_set_tick_callback(state->app, nullptr, nullptr);
        (void)kavio_app_set_stopping_callback(state->app, nullptr, nullptr);
        if (!state->running) (void)kavio_app_destroy(state->app);
        state->app = nullptr;
    }
    if (state->lua) {
        if (state->ready_ref != LUA_NOREF) luaL_unref(state->lua, LUA_REGISTRYINDEX, state->ready_ref);
        if (state->tick_ref != LUA_NOREF) luaL_unref(state->lua, LUA_REGISTRYINDEX, state->tick_ref);
        if (state->stopping_ref != LUA_NOREF) luaL_unref(state->lua, LUA_REGISTRYINDEX, state->stopping_ref);
    }
    delete state;
}

}  // namespace kavio::lua_binding

/** Return monotonic seconds for interval measurements (not wall time). */
static int LuaMonotonicSeconds(lua_State* L)
{
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    lua_pushnumber(L, static_cast<lua_Number>(seconds));
    return 1;
}

extern "C" int luaopen_kavio_native(lua_State* L)
{
    CreateMetatable(L, kl::kAppMetatable, kAppMethods, AppGc);
    CreateMetatable(L, kl::kPresentationMetatable, kPresentationMethods, PresentationGc);
    CreateMetatable(L, kl::kFrameMetatable, kFrameMethods, FrameGc);
    CreateMetatable(L, kl::kBufferMetatable, kBufferMethods, BufferGc, BufferLen);
    CreateMetatable(L, kl::kDeviceMetatable, kDeviceMethods, DeviceGc);
    CreateMetatable(L, kl::kImageFrameMetatable, kImageMethods, ImageGc);
    CreateMetatable(L, kl::kTxSurfaceMetatable, kTxMethods, TxGc);
    CreateMetatable(L, kl::kH264PacketMetatable, kH264Methods, H264Gc);

    lua_newtable(L);
    lua_pushcfunction(L, AppCreate); lua_setfield(L, -2, "app_create");
    lua_pushcfunction(L, LuaMonotonicSeconds); lua_setfield(L, -2, "monotonic_seconds");
    lua_pushinteger(L, KAVIO_ABI_VERSION); lua_setfield(L, -2, "ABI_VERSION");
    lua_pushinteger(L, KAVIO_PIXEL_FORMAT_UNKNOWN); lua_setfield(L, -2, "PIXEL_FORMAT_UNKNOWN");
    lua_pushinteger(L, KAVIO_PIXEL_FORMAT_UYVY); lua_setfield(L, -2, "PIXEL_FORMAT_UYVY");
    lua_pushinteger(L, KAVIO_PIXEL_FORMAT_YUYV); lua_setfield(L, -2, "PIXEL_FORMAT_YUYV");
    lua_pushinteger(L, KAVIO_PIXEL_FORMAT_BGRA8888); lua_setfield(L, -2, "PIXEL_FORMAT_BGRA8888");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_UNKNOWN); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_UNKNOWN");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_NOT_CONNECTED); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_NOT_CONNECTED");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_CONNECTED); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_CONNECTED");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_RUNNING); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_RUNNING");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_STOPPED); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_STOPPED");
    lua_pushinteger(L, KAVIO_RUNTIME_VIDEO_STATUS_ERROR); lua_setfield(L, -2, "RUNTIME_VIDEO_STATUS_ERROR");
    lua_pushinteger(L, KAVIO_INPUT_MODE_BLE); lua_setfield(L, -2, "INPUT_MODE_BLE");
    lua_pushinteger(L, KAVIO_INPUT_MODE_RF24); lua_setfield(L, -2, "INPUT_MODE_RF24");
    lua_pushinteger(L, KAVIO_INPUT_MODE_USB); lua_setfield(L, -2, "INPUT_MODE_USB");
    lua_pushinteger(L, KAVIO_INPUT_CHANNEL_1); lua_setfield(L, -2, "INPUT_CHANNEL_1");
    lua_pushinteger(L, KAVIO_INPUT_CHANNEL_2); lua_setfield(L, -2, "INPUT_CHANNEL_2");
    lua_pushinteger(L, KAVIO_INPUT_CHANNEL_3); lua_setfield(L, -2, "INPUT_CHANNEL_3");
    lua_pushinteger(L, KAVIO_INPUT_CONNECTION_STATUS_SYNCING); lua_setfield(L, -2, "INPUT_CONNECTION_STATUS_SYNCING");
    lua_pushinteger(L, KAVIO_INPUT_CONNECTION_STATUS_PAIRING); lua_setfield(L, -2, "INPUT_CONNECTION_STATUS_PAIRING");
    lua_pushinteger(L, KAVIO_INPUT_CONNECTION_STATUS_CONNECTED); lua_setfield(L, -2, "INPUT_CONNECTION_STATUS_CONNECTED");
    lua_pushinteger(L, KAVIO_INPUT_CONNECTION_STATUS_DISCONNECTED); lua_setfield(L, -2, "INPUT_CONNECTION_STATUS_DISCONNECTED");
    lua_pushinteger(L, KAVIO_H264_PACKET_KEY_FRAME); lua_setfield(L, -2, "H264_PACKET_KEY_FRAME");
    lua_pushinteger(L, KAVIO_H264_PACKET_IDR); lua_setfield(L, -2, "H264_PACKET_IDR");
    lua_pushinteger(L, KAVIO_H264_PACKET_SPS); lua_setfield(L, -2, "H264_PACKET_SPS");
    lua_pushinteger(L, KAVIO_H264_PACKET_PPS); lua_setfield(L, -2, "H264_PACKET_PPS");
    lua_pushinteger(L, KAVIO_H264_PACKET_HEADER); lua_setfield(L, -2, "H264_PACKET_HEADER");
    lua_newtable(L);
    for (const auto& key : kavio::detail::kKeyCodeDescriptors) {
        lua_pushinteger(L, static_cast<lua_Integer>(kavio::KeyCodeValue(key.code)));
        lua_setfield(L, -2, key.name);
    }
    lua_setfield(L, -2, "KEY_CODES");
    lua_pushinteger(L, KAVIO_KEY_OPTION_DOWN); lua_setfield(L, -2, "KEY_DOWN");
    lua_pushinteger(L, KAVIO_KEY_OPTION_UP); lua_setfield(L, -2, "KEY_UP");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_MOVE); lua_setfield(L, -2, "MOUSE_MOVE");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_LBUTTON_DOWN); lua_setfield(L, -2, "MOUSE_LBUTTON_DOWN");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_LBUTTON_UP); lua_setfield(L, -2, "MOUSE_LBUTTON_UP");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_RBUTTON_DOWN); lua_setfield(L, -2, "MOUSE_RBUTTON_DOWN");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_RBUTTON_UP); lua_setfield(L, -2, "MOUSE_RBUTTON_UP");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_MBUTTON_DOWN); lua_setfield(L, -2, "MOUSE_MBUTTON_DOWN");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_MBUTTON_UP); lua_setfield(L, -2, "MOUSE_MBUTTON_UP");
    lua_pushinteger(L, KAVIO_MOUSE_OPTION_WHEEL); lua_setfield(L, -2, "MOUSE_WHEEL");
    return 1;
}
