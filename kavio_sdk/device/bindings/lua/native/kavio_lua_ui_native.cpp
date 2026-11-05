#include "KavioLuaInternal.h"

#include <kavio_c/ui.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace kl = kavio::lua_binding;

namespace {

struct WidgetState {
    lua_State* lua = nullptr;
    KavioWidget* widget = nullptr;
    int click_ref = LUA_NOREF;
    int value_ref = LUA_NOREF;
    int checked_ref = LUA_NOREF;
};

struct BackendState {
    KavioUiBackend* backend = nullptr;
    int presentation_ref = LUA_NOREF;
};

constexpr const char* kPendingUiError = "kavio.ui.pending_error";

template <typename T>
T* CheckState(lua_State* L, int index, const char* metatable)
{
    auto** holder = static_cast<T**>(luaL_checkudata(L, index, metatable));
    if (!holder || !*holder) luaL_error(L, "Kavio UI object is closed");
    return *holder;
}

template <typename T>
void NewHolder(lua_State* L, T* value, const char* metatable)
{
    auto** holder = static_cast<T**>(lua_newuserdatauv(L, sizeof(T*), 0));
    *holder = value;
    luaL_getmetatable(L, metatable);
    lua_setmetatable(L, -2);
}

int RaiseUi(lua_State* L, KavioStatusCode status, bool backend = false)
{
    if (status == KAVIO_STATUS_OK) return 0;
    const char* message = backend ? kavio_ui_backend_last_error_message() : kavio_ui_last_error_message();
    return luaL_error(L, "Kavio UI error %d: %s", static_cast<int>(status), message && *message ? message : "UI operation failed");
}

void SetPendingError(lua_State* L)
{
    const char* message = lua_tostring(L, -1);
    lua_pushstring(L, message ? message : "Lua UI callback failed");
    lua_setfield(L, LUA_REGISTRYINDEX, kPendingUiError);
    lua_pop(L, 1);
}

int RaisePendingError(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kPendingUiError);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 0; }
    const char* message = lua_tostring(L, -1);
    std::string copy = message ? message : "Lua UI callback failed";
    lua_pop(L, 1);
    lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, kPendingUiError);
    return luaL_error(L, "%s", copy.c_str());
}

void CallWidgetRef(WidgetState* state, int ref, int nargs = 0)
{
    if (!state || ref == LUA_NOREF) {
        if (state && nargs > 0) lua_pop(state->lua, nargs);
        return;
    }
    lua_State* L = state->lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (nargs > 0) lua_insert(L, -(nargs + 1));
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) SetPendingError(L);
}

void ClickBridge(void* user_data) { auto* s=static_cast<WidgetState*>(user_data); CallWidgetRef(s,s?s->click_ref:LUA_NOREF); }
void ValueBridge(int32_t value, void* user_data) { auto*s=static_cast<WidgetState*>(user_data); if(!s)return;lua_pushinteger(s->lua,value);CallWidgetRef(s,s->value_ref,1); }
void CheckedBridge(uint8_t value, void* user_data) { auto*s=static_cast<WidgetState*>(user_data);if(!s)return;lua_pushboolean(s->lua,value);CallWidgetRef(s,s->checked_ref,1); }

void ReplaceRef(lua_State* L, int& slot, int index)
{
    if (slot != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, slot); slot=LUA_NOREF; }
    if (!lua_isnoneornil(L,index)) { lua_pushvalue(L,index); slot=luaL_ref(L,LUA_REGISTRYINDEX); }
}

int WidgetGc(lua_State* L)
{
    auto** holder=static_cast<WidgetState**>(luaL_checkudata(L,1,kl::kWidgetMetatable));
    if(holder&&*holder){auto*s=*holder;if(s->widget){(void)kavio_widget_set_click_callback(s->widget,nullptr,nullptr);(void)kavio_widget_set_value_callback(s->widget,nullptr,nullptr);(void)kavio_widget_set_checked_callback(s->widget,nullptr,nullptr);kavio_widget_destroy(s->widget);}for(int*ref:{&s->click_ref,&s->value_ref,&s->checked_ref})if(*ref!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,*ref);delete s;*holder=nullptr;}return 0;
}

int WidgetCreate(lua_State* L)
{
    const auto type=static_cast<KavioWidgetType>(luaL_checkinteger(L,1));
    const char* text=luaL_optstring(L,2,"");
    const int32_t min=static_cast<int32_t>(luaL_optinteger(L,3,0));
    const int32_t max=static_cast<int32_t>(luaL_optinteger(L,4,100));
    const int32_t value=static_cast<int32_t>(luaL_optinteger(L,5,0));
    const uint8_t checked=lua_toboolean(L,6)?1u:0u;
    KavioWidget* widget=nullptr;const auto status=kavio_widget_create(type,text,min,max,value,checked,&widget);if(status!=KAVIO_STATUS_OK)return RaiseUi(L,status);
    auto*state=new(std::nothrow)WidgetState();if(!state){kavio_widget_destroy(widget);return luaL_error(L,"memory allocation failed");}state->lua=L;state->widget=widget;NewHolder(L,state,kl::kWidgetMetatable);return 1;
}

int WidgetAdd(lua_State* L){auto*p=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto*c=CheckState<WidgetState>(L,2,kl::kWidgetMetatable);auto st=kavio_widget_add(p->widget,c->widget);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}

int WidgetSetStyle(lua_State* L)
{
    auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);luaL_checktype(L,2,LUA_TTABLE);KavioUiStyle style{};kavio_ui_style_init(&style);
#define READ_INT_FIELD(name, field) do { lua_getfield(L,2,name); if(!lua_isnil(L,-1)) style.field=static_cast<decltype(style.field)>(luaL_checkinteger(L,-1)); lua_pop(L,1); } while(0)
    READ_INT_FIELD("width",width);READ_INT_FIELD("height",height);READ_INT_FIELD("padding",padding);READ_INT_FIELD("gap",gap);READ_INT_FIELD("radius",radius);READ_INT_FIELD("font_size",font_size);READ_INT_FIELD("opacity",opacity);
#undef READ_INT_FIELD
    lua_getfield(L,2,"background");if(!lua_isnil(L,-1)){style.has_background=1u;style.background=static_cast<uint32_t>(luaL_checkinteger(L,-1));}lua_pop(L,1);
    lua_getfield(L,2,"text_color");if(!lua_isnil(L,-1)){style.has_text_color=1u;style.text_color=static_cast<uint32_t>(luaL_checkinteger(L,-1));}lua_pop(L,1);
    auto st=kavio_widget_set_style(s->widget,&style);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;
}

int WidgetSetText(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto st=kavio_widget_set_text(s->widget,luaL_checkstring(L,2));if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}
int WidgetSetSource(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto st=kavio_widget_set_source(s->widget,luaL_checkstring(L,2));if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}
int WidgetSetBuffer(lua_State* L)
{
    auto* widget = CheckState<WidgetState>(L, 1, kl::kWidgetMetatable);
    auto* frame = CheckState<kl::ImageFrameState>(L, 2, kl::kImageFrameMetatable);
    const uint32_t max_width = static_cast<uint32_t>(luaL_optinteger(L, 3, 620));
    const uint32_t max_height = static_cast<uint32_t>(luaL_optinteger(L, 4, 320));
    KavioImageFrameInfo info{};
    kavio_image_frame_info_init(&info);
    auto status = kavio_image_frame_get_info(frame->frame, &info);
    if (status != KAVIO_STATUS_OK) return RaiseUi(L, status);
    const uint8_t* bytes = nullptr;
    uint64_t size = 0;
    status = kavio_image_frame_get_data(frame->frame, &bytes, &size);
    if (status != KAVIO_STATUS_OK) return RaiseUi(L, status);
    status = kavio_widget_set_image_buffer(widget->widget, bytes, size,
        info.width, info.height, info.stride, info.pixel_format, max_width, max_height);
    if (status != KAVIO_STATUS_OK) return RaiseUi(L, status);
    return 0;
}

int WidgetSetRange(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto st=kavio_widget_set_range(s->widget,static_cast<int32_t>(luaL_checkinteger(L,2)),static_cast<int32_t>(luaL_checkinteger(L,3)));if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}
int WidgetSetValue(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto st=kavio_widget_set_value(s->widget,static_cast<int32_t>(luaL_checkinteger(L,2)));if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}
int WidgetGetValue(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);int32_t v=0;auto st=kavio_widget_get_value(s->widget,&v);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);lua_pushinteger(L,v);return 1;}
int WidgetSetChecked(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);auto st=kavio_widget_set_checked(s->widget,lua_toboolean(L,2)?1u:0u);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;}
int WidgetGetChecked(lua_State*L){auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);uint8_t v=0;auto st=kavio_widget_get_checked(s->widget,&v);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);lua_pushboolean(L,v);return 1;}

int WidgetCallback(lua_State*L,int kind)
{
    auto*s=CheckState<WidgetState>(L,1,kl::kWidgetMetatable);if(!lua_isnoneornil(L,2)&&!lua_isfunction(L,2))return luaL_error(L,"callback must be a function or nil");KavioStatusCode st=KAVIO_STATUS_OK;
    if(kind==0){ReplaceRef(L,s->click_ref,2);st=kavio_widget_set_click_callback(s->widget,s->click_ref==LUA_NOREF?nullptr:ClickBridge,s);}else if(kind==1){ReplaceRef(L,s->value_ref,2);st=kavio_widget_set_value_callback(s->widget,s->value_ref==LUA_NOREF?nullptr:ValueBridge,s);}else{ReplaceRef(L,s->checked_ref,2);st=kavio_widget_set_checked_callback(s->widget,s->checked_ref==LUA_NOREF?nullptr:CheckedBridge,s);}if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st);return 0;
}
int WidgetClick(lua_State*L){return WidgetCallback(L,0);}int WidgetValueCallback(lua_State*L){return WidgetCallback(L,1);}int WidgetCheckedCallback(lua_State*L){return WidgetCallback(L,2);}

int BackendGc(lua_State*L){auto**h=static_cast<BackendState**>(luaL_checkudata(L,1,kl::kBackendMetatable));if(h&&*h){if((*h)->backend){kavio_ui_backend_shutdown((*h)->backend);kavio_ui_backend_destroy((*h)->backend);}if((*h)->presentation_ref!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,(*h)->presentation_ref);delete*h;*h=nullptr;}return 0;}
int BackendCreate(lua_State*L){KavioUiBackend*b=nullptr;auto st=kavio_ui_backend_create(&b);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st,true);auto*s=new(std::nothrow)BackendState{b,LUA_NOREF};if(!s){kavio_ui_backend_destroy(b);return luaL_error(L,"memory allocation failed");}NewHolder(L,s,kl::kBackendMetatable);return 1;}
int BackendInitialize(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);auto*p=CheckState<kl::PresentationState>(L,2,kl::kPresentationMetatable);auto st=kavio_ui_backend_initialize(b->backend,p->presentation);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st,true);if(b->presentation_ref!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,b->presentation_ref);lua_pushvalue(L,2);b->presentation_ref=luaL_ref(L,LUA_REGISTRYINDEX);return 0;}
int BackendShutdown(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);kavio_ui_backend_shutdown(b->backend);if(b->presentation_ref!=LUA_NOREF){luaL_unref(L,LUA_REGISTRYINDEX,b->presentation_ref);b->presentation_ref=LUA_NOREF;}return 0;}
int BackendProcess(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);auto st=kavio_ui_backend_process(b->backend);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st,true);return RaisePendingError(L);}
int BackendSetContent(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);auto*w=CheckState<WidgetState>(L,2,kl::kWidgetMetatable);auto st=kavio_ui_backend_set_content(b->backend,w->widget);if(st!=KAVIO_STATUS_OK)return RaiseUi(L,st,true);return 0;}
int BackendClear(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);kavio_ui_backend_clear_content(b->backend);return 0;}
int BackendReady(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);lua_pushboolean(L,kavio_ui_backend_is_ready(b->backend));return 1;}
int BackendForeground(lua_State*L){auto*b=CheckState<BackendState>(L,1,kl::kBackendMetatable);lua_pushboolean(L,kavio_ui_backend_is_foreground(b->backend));return 1;}

void CreateMetatable(lua_State*L,const char*name,const luaL_Reg*methods,lua_CFunction gc){luaL_newmetatable(L,name);kl::SetMethods(L,methods);if(gc){lua_pushcfunction(L,gc);lua_setfield(L,-2,"__gc");}lua_pop(L,1);}
const luaL_Reg kWidgetMethods[]={{"add",WidgetAdd},{"set_style",WidgetSetStyle},{"set_text",WidgetSetText},{"set_source",WidgetSetSource},{"set_buffer",WidgetSetBuffer},{"set_range",WidgetSetRange},{"set_value",WidgetSetValue},{"get_value",WidgetGetValue},{"set_checked",WidgetSetChecked},{"get_checked",WidgetGetChecked},{"set_click_callback",WidgetClick},{"set_value_callback",WidgetValueCallback},{"set_checked_callback",WidgetCheckedCallback},{nullptr,nullptr}};
const luaL_Reg kBackendMethods[]={{"initialize",BackendInitialize},{"shutdown",BackendShutdown},{"process",BackendProcess},{"set_content",BackendSetContent},{"clear_content",BackendClear},{"is_ready",BackendReady},{"is_foreground",BackendForeground},{nullptr,nullptr}};

}  // namespace

extern "C" int luaopen_kavio_ui_native(lua_State*L)
{
    CreateMetatable(L,kl::kWidgetMetatable,kWidgetMethods,WidgetGc);CreateMetatable(L,kl::kBackendMetatable,kBackendMethods,BackendGc);
    lua_newtable(L);lua_pushcfunction(L,WidgetCreate);lua_setfield(L,-2,"widget_create");lua_pushcfunction(L,BackendCreate);lua_setfield(L,-2,"backend_create");
    lua_pushinteger(L,KAVIO_WIDGET_PAGE);lua_setfield(L,-2,"PAGE");lua_pushinteger(L,KAVIO_WIDGET_COLUMN);lua_setfield(L,-2,"COLUMN");lua_pushinteger(L,KAVIO_WIDGET_ROW);lua_setfield(L,-2,"ROW");lua_pushinteger(L,KAVIO_WIDGET_LABEL);lua_setfield(L,-2,"LABEL");lua_pushinteger(L,KAVIO_WIDGET_BUTTON);lua_setfield(L,-2,"BUTTON");lua_pushinteger(L,KAVIO_WIDGET_IMAGE);lua_setfield(L,-2,"IMAGE");lua_pushinteger(L,KAVIO_WIDGET_SLIDER);lua_setfield(L,-2,"SLIDER");lua_pushinteger(L,KAVIO_WIDGET_SWITCH);lua_setfield(L,-2,"SWITCH");return 1;
}
