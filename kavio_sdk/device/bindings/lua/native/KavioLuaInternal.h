#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <kavio_c/app.h>
#include <kavio_c/device.h>
#include <kavio_c/presentation.h>

#include <cstdint>
#include <string>

namespace kavio::lua_binding {

inline constexpr const char* kAppMetatable = "kavio.App";
inline constexpr const char* kPresentationMetatable = "kavio.Presentation";
inline constexpr const char* kFrameMetatable = "kavio.FrameLease";
inline constexpr const char* kBufferMetatable = "kavio.BufferView";
inline constexpr const char* kDeviceMetatable = "kavio.Device";
inline constexpr const char* kImageFrameMetatable = "kavio.ImageFrame";
inline constexpr const char* kTxSurfaceMetatable = "kavio.TxSurface";
inline constexpr const char* kH264PacketMetatable = "kavio.H264Packet";
inline constexpr const char* kWidgetMetatable = "kavio.Widget";
inline constexpr const char* kBackendMetatable = "kavio.LvglBackend";

struct AppState {
    lua_State* lua = nullptr;
    KavioApp* app = nullptr;
    int references = 1;
    int ready_ref = LUA_NOREF;
    int tick_ref = LUA_NOREF;
    int stopping_ref = LUA_NOREF;
    bool running = false;
    std::string pending_error;
};

struct ImageFrameState {
    KavioImageFrame* frame = nullptr;
    AppState* owner = nullptr;
};

struct PresentationState {
    KavioPresentation* presentation = nullptr;
    AppState* owner = nullptr;
    int touch_ref = LUA_NOREF;
    int visibility_ref = LUA_NOREF;
};

inline void RetainApp(AppState* state)
{
    if (state) ++state->references;
}

void ReleaseApp(AppState* state);
void CaptureAppError(AppState* state, const char* message);

inline void ReplaceLuaRef(lua_State* L, int& slot, int index)
{
    if (slot != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot);
        slot = LUA_NOREF;
    }
    if (!lua_isnoneornil(L, index)) {
        lua_pushvalue(L, index);
        slot = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

inline void SetMethods(lua_State* L, const luaL_Reg* methods)
{
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
}

}  // namespace kavio::lua_binding
