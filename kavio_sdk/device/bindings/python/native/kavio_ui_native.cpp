#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstring>
#include <new>

#include <kavio_c/ui.h>

#ifndef PyCFunction_CAST
#define PyCFunction_CAST(func) reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(func))
#endif

namespace {
PyObject* g_error = nullptr;
PyObject* g_callback_type = nullptr;
PyObject* g_callback_value = nullptr;
PyObject* g_callback_traceback = nullptr;

void CaptureUiCallbackException()
{
    if (!PyErr_Occurred()) return;
    if (!g_callback_type) {
        PyErr_Fetch(&g_callback_type, &g_callback_value, &g_callback_traceback);
        PyErr_NormalizeException(&g_callback_type, &g_callback_value, &g_callback_traceback);
    } else {
        PyErr_Clear();
    }
}

bool RestoreUiCallbackException()
{
    if (!g_callback_type) return false;
    PyObject* type = g_callback_type;
    PyObject* value = g_callback_value;
    PyObject* traceback = g_callback_traceback;
    g_callback_type = g_callback_value = g_callback_traceback = nullptr;
    PyErr_Restore(type, value, traceback);
    return true;
}

bool RaiseUi(KavioStatusCode status, bool backend = false)
{
    if (status == KAVIO_STATUS_OK) return true;
    const char* message = backend ? kavio_ui_backend_last_error_message() : kavio_ui_last_error_message();
    PyErr_Format(g_error ? g_error : PyExc_RuntimeError,
                 "Kavio UI error %d: %s", static_cast<int>(status),
                 message && *message ? message : "UI operation failed");
    return false;
}

struct WidgetState {
    KavioWidget* widget = nullptr;
    PyObject* click = nullptr;
    PyObject* value = nullptr;
    PyObject* checked = nullptr;
};

WidgetState* GetWidget(PyObject* capsule)
{
    return static_cast<WidgetState*>(PyCapsule_GetPointer(capsule, "kavio.widget"));
}

void WidgetDestroy(PyObject* capsule)
{
    auto* state = static_cast<WidgetState*>(PyCapsule_GetPointer(capsule, "kavio.widget"));
    if (!state) { PyErr_Clear(); return; }
    kavio_widget_destroy(state->widget);
    Py_XDECREF(state->click);
    Py_XDECREF(state->value);
    Py_XDECREF(state->checked);
    delete state;
}

void Replace(PyObject*& slot, PyObject* callback)
{
    Py_XINCREF(callback);
    Py_XDECREF(slot);
    slot = callback;
}

void ClickBridge(void* user_data)
{
    auto* state = static_cast<WidgetState*>(user_data);
    if (!state || !state->click || state->click == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunctionObjArgs(state->click, nullptr);
    if (!result) CaptureUiCallbackException();
    Py_XDECREF(result);
    PyGILState_Release(gil);
}

void ValueBridge(int32_t value, void* user_data)
{
    auto* state = static_cast<WidgetState*>(user_data);
    if (!state || !state->value || state->value == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* arg = PyLong_FromLong(value);
    PyObject* result = arg ? PyObject_CallFunctionObjArgs(state->value, arg, nullptr) : nullptr;
    if (!result) CaptureUiCallbackException();
    Py_XDECREF(result); Py_XDECREF(arg);
    PyGILState_Release(gil);
}

void CheckedBridge(uint8_t checked, void* user_data)
{
    auto* state = static_cast<WidgetState*>(user_data);
    if (!state || !state->checked || state->checked == Py_None) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* arg = checked ? Py_True : Py_False;
    Py_INCREF(arg);
    PyObject* result = PyObject_CallFunctionObjArgs(state->checked, arg, nullptr);
    if (!result) CaptureUiCallbackException();
    Py_XDECREF(result); Py_DECREF(arg);
    PyGILState_Release(gil);
}

struct BackendState {
    KavioUiBackend* backend = nullptr;
    PyObject* presentation_owner = nullptr;
};

BackendState* GetBackend(PyObject* capsule)
{
    return static_cast<BackendState*>(PyCapsule_GetPointer(capsule, "kavio.ui_backend"));
}

void BackendDestroy(PyObject* capsule)
{
    auto* state = static_cast<BackendState*>(PyCapsule_GetPointer(capsule, "kavio.ui_backend"));
    if (!state) { PyErr_Clear(); return; }
    if (state->backend) {
        kavio_ui_backend_shutdown(state->backend);
        kavio_ui_backend_destroy(state->backend);
    }
    Py_XDECREF(state->presentation_owner);
    delete state;
}

PyObject* WidgetCreate(PyObject*, PyObject* args, PyObject* kwargs)
{
    static const char* names[] = {"type", "text", "minimum", "maximum", "value", "checked", nullptr};
    int type = 0;
    const char* text = nullptr;
    int minimum = 0, maximum = 100, value = 0, checked = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|ziiip", const_cast<char**>(names),
                                     &type, &text, &minimum, &maximum, &value, &checked)) return nullptr;
    auto* state = new (std::nothrow) WidgetState();
    if (!state) return PyErr_NoMemory();
    const KavioStatusCode status = kavio_widget_create(
        static_cast<KavioWidgetType>(type), text, minimum, maximum, value,
        checked ? 1u : 0u, &state->widget);
    if (!RaiseUi(status)) { delete state; return nullptr; }
    PyObject* capsule = PyCapsule_New(state, "kavio.widget", WidgetDestroy);
    if (!capsule) { kavio_widget_destroy(state->widget); delete state; return nullptr; }
    return capsule;
}

PyObject* WidgetAdd(PyObject*, PyObject* args)
{
    PyObject* parent_obj = nullptr; PyObject* child_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &parent_obj, &child_obj)) return nullptr;
    auto* parent = GetWidget(parent_obj); auto* child = GetWidget(child_obj);
    if (!parent || !child) return nullptr;
    if (!RaiseUi(kavio_widget_add(parent->widget, child->widget))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* WidgetSetStyle(PyObject*, PyObject* args, PyObject* kwargs)
{
    static const char* names[] = {
        "widget", "width", "height", "padding", "gap", "radius", "font_size",
        "background", "text_color", "opacity", nullptr};
    PyObject* widget_obj = nullptr;
    int width=-1,height=-1,padding=-1,gap=-1,radius=-1,font_size=-1;
    PyObject* background=Py_None; PyObject* text_color=Py_None; unsigned int opacity=255;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|iiiiiiOOI", const_cast<char**>(names),
                                     &widget_obj,&width,&height,&padding,&gap,&radius,&font_size,
                                     &background,&text_color,&opacity)) return nullptr;
    auto* state=GetWidget(widget_obj); if(!state) return nullptr;
    KavioUiStyle style{}; kavio_ui_style_init(&style);
    style.width=width; style.height=height; style.padding=padding; style.gap=gap;
    style.radius=radius; style.font_size=font_size; style.opacity=static_cast<uint8_t>(opacity>255?255:opacity);
    if(background!=Py_None){ unsigned long value=PyLong_AsUnsignedLong(background); if(PyErr_Occurred()) return nullptr; style.has_background=1u; style.background=static_cast<uint32_t>(value); }
    if(text_color!=Py_None){ unsigned long value=PyLong_AsUnsignedLong(text_color); if(PyErr_Occurred()) return nullptr; style.has_text_color=1u; style.text_color=static_cast<uint32_t>(value); }
    if(!RaiseUi(kavio_widget_set_style(state->widget,&style))) return nullptr;
    Py_RETURN_NONE;
}

PyObject* WidgetSetText(PyObject*,PyObject* args){PyObject* o=nullptr;const char* v=nullptr;if(!PyArg_ParseTuple(args,"Os",&o,&v))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;if(!RaiseUi(kavio_widget_set_text(s->widget,v)))return nullptr;Py_RETURN_NONE;}
// Accepts an SDK image-frame handle. The C API copies pixels before return.
PyObject* WidgetSetBuffer(PyObject*, PyObject* args)
{
    PyObject* widget_obj = nullptr;
    PyObject* frame_obj = nullptr;
    unsigned int max_width = 620, max_height = 320;
    if (!PyArg_ParseTuple(args, "OO|II", &widget_obj, &frame_obj,
                          &max_width, &max_height)) return nullptr;
    auto* widget = GetWidget(widget_obj);
    if (!widget) return nullptr;
    PyObject* info = PyObject_GetAttrString(frame_obj, "info");
    if (!info) return nullptr;
    if (PyCallable_Check(info)) {
        PyObject* result = PyObject_CallObject(info, nullptr);
        Py_DECREF(info);
        info = result;
        if (!info) return nullptr;
    }
    if (!PyDict_Check(info)) {
        Py_DECREF(info);
        PyErr_SetString(PyExc_TypeError, "image frame info must be a dictionary");
        return nullptr;
    }
    auto get_number = [&](const char* name, uint32_t& value) -> bool {
        PyObject* item = PyDict_GetItemString(info, name);
        if (!item) { PyErr_Format(PyExc_KeyError, "image frame info missing %s", name); return false; }
        const unsigned long n = PyLong_AsUnsignedLong(item);
        if (PyErr_Occurred() || n > UINT32_MAX) return false;
        value = static_cast<uint32_t>(n);
        return true;
    };
    uint32_t width=0, height=0, stride=0, pixel_format=0;
    const bool valid = get_number("width", width) && get_number("height", height) &&
                       get_number("stride", stride) && get_number("pixel_format", pixel_format);
    Py_DECREF(info);
    if (!valid) return nullptr;
    Py_buffer view{};
    if (PyObject_GetBuffer(frame_obj, &view, PyBUF_SIMPLE) != 0) return nullptr;
    const auto status = kavio_widget_set_image_buffer(widget->widget,
        static_cast<const uint8_t*>(view.buf), static_cast<uint64_t>(view.len),
        width, height, stride, pixel_format, max_width, max_height);
    PyBuffer_Release(&view);
    if (!RaiseUi(status)) return nullptr;
    Py_RETURN_NONE;
}

PyObject* WidgetSetSource(PyObject*,PyObject* args){PyObject* o=nullptr;const char* v=nullptr;if(!PyArg_ParseTuple(args,"Os",&o,&v))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;if(!RaiseUi(kavio_widget_set_source(s->widget,v)))return nullptr;Py_RETURN_NONE;}
PyObject* WidgetSetRange(PyObject*,PyObject* args){PyObject* o=nullptr;int a=0,b=0;if(!PyArg_ParseTuple(args,"Oii",&o,&a,&b))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;if(!RaiseUi(kavio_widget_set_range(s->widget,a,b)))return nullptr;Py_RETURN_NONE;}
PyObject* WidgetSetValue(PyObject*,PyObject* args){PyObject* o=nullptr;int v=0;if(!PyArg_ParseTuple(args,"Oi",&o,&v))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;if(!RaiseUi(kavio_widget_set_value(s->widget,v)))return nullptr;Py_RETURN_NONE;}
PyObject* WidgetGetValue(PyObject*,PyObject* args){PyObject* o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;int32_t v=0;if(!RaiseUi(kavio_widget_get_value(s->widget,&v)))return nullptr;return PyLong_FromLong(v);}
PyObject* WidgetSetChecked(PyObject*,PyObject* args){PyObject* o=nullptr;int v=0;if(!PyArg_ParseTuple(args,"Op",&o,&v))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;if(!RaiseUi(kavio_widget_set_checked(s->widget,v?1u:0u)))return nullptr;Py_RETURN_NONE;}
PyObject* WidgetGetChecked(PyObject*,PyObject* args){PyObject* o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*s=GetWidget(o);if(!s)return nullptr;uint8_t v=0;if(!RaiseUi(kavio_widget_get_checked(s->widget,&v)))return nullptr;return PyBool_FromLong(v);}

PyObject* WidgetSetCallback(PyObject* args,const char* kind)
{
    PyObject* o=nullptr;PyObject* cb=nullptr;if(!PyArg_ParseTuple(args,"OO",&o,&cb))return nullptr;
    if(cb!=Py_None&&!PyCallable_Check(cb)){PyErr_SetString(PyExc_TypeError,"callback must be callable or None");return nullptr;}
    auto*s=GetWidget(o);if(!s)return nullptr;KavioStatusCode status=KAVIO_STATUS_OK;
    if(std::strcmp(kind,"click")==0){Replace(s->click,cb);status=kavio_widget_set_click_callback(s->widget,cb==Py_None?nullptr:ClickBridge,cb==Py_None?nullptr:s);}
    else if(std::strcmp(kind,"value")==0){Replace(s->value,cb);status=kavio_widget_set_value_callback(s->widget,cb==Py_None?nullptr:ValueBridge,cb==Py_None?nullptr:s);}
    else{Replace(s->checked,cb);status=kavio_widget_set_checked_callback(s->widget,cb==Py_None?nullptr:CheckedBridge,cb==Py_None?nullptr:s);}
    if (!RaiseUi(status)) return nullptr;
    Py_RETURN_NONE;
}
PyObject* WidgetSetClick(PyObject*,PyObject*a){return WidgetSetCallback(a,"click");}
PyObject* WidgetSetValueCallback(PyObject*,PyObject*a){return WidgetSetCallback(a,"value");}
PyObject* WidgetSetCheckedCallback(PyObject*,PyObject*a){return WidgetSetCallback(a,"checked");}

PyObject* BackendCreate(PyObject*,PyObject*)
{
    auto* state=new(std::nothrow) BackendState();if(!state)return PyErr_NoMemory();
    if(!RaiseUi(kavio_ui_backend_create(&state->backend),true)){delete state;return nullptr;}
    PyObject* capsule=PyCapsule_New(state,"kavio.ui_backend",BackendDestroy);
    if(!capsule){kavio_ui_backend_destroy(state->backend);delete state;return nullptr;}return capsule;
}

PyObject* BackendInitialize(PyObject*,PyObject* args)
{
    PyObject* backend_obj=nullptr;PyObject* presentation_obj=nullptr;
    if(!PyArg_ParseTuple(args,"OO",&backend_obj,&presentation_obj))return nullptr;
    auto* backend=GetBackend(backend_obj);if(!backend)return nullptr;
    if(!PyCapsule_IsValid(presentation_obj,"kavio.presentation")){
        PyErr_SetString(PyExc_TypeError,"presentation must be a Kavio Presentation handle");
        return nullptr;
    }
    auto* presentation=static_cast<KavioPresentation*>(PyCapsule_GetContext(presentation_obj));
    if(!presentation){
        if(!PyErr_Occurred())PyErr_SetString(PyExc_RuntimeError,"Kavio Presentation handle is unavailable");
        return nullptr;
    }
    if(!RaiseUi(kavio_ui_backend_initialize(backend->backend,presentation),true))return nullptr;
    Py_XINCREF(presentation_obj);Py_XDECREF(backend->presentation_owner);backend->presentation_owner=presentation_obj;Py_RETURN_NONE;
}
PyObject* BackendShutdown(PyObject*,PyObject* args){PyObject*o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*b=GetBackend(o);if(!b)return nullptr;kavio_ui_backend_shutdown(b->backend);Py_CLEAR(b->presentation_owner);Py_RETURN_NONE;}
PyObject* BackendProcess(PyObject*, PyObject* args)
{
    PyObject* object = nullptr;
    if (!PyArg_ParseTuple(args, "O", &object)) return nullptr;
    auto* backend = GetBackend(object);
    if (!backend) return nullptr;
    if (!RaiseUi(kavio_ui_backend_process(backend->backend), true)) return nullptr;
    if (RestoreUiCallbackException()) return nullptr;
    Py_RETURN_NONE;
}
PyObject* BackendSetContent(PyObject*,PyObject* args){PyObject*bobj=nullptr;PyObject*wobj=nullptr;if(!PyArg_ParseTuple(args,"OO",&bobj,&wobj))return nullptr;auto*b=GetBackend(bobj);auto*w=GetWidget(wobj);if(!b||!w)return nullptr;if(!RaiseUi(kavio_ui_backend_set_content(b->backend,w->widget),true))return nullptr;Py_RETURN_NONE;}
PyObject* BackendClear(PyObject*,PyObject* args){PyObject*o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*b=GetBackend(o);if(!b)return nullptr;kavio_ui_backend_clear_content(b->backend);Py_RETURN_NONE;}
PyObject* BackendIsReady(PyObject*,PyObject* args){PyObject*o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*b=GetBackend(o);if(!b)return nullptr;return PyBool_FromLong(kavio_ui_backend_is_ready(b->backend));}
PyObject* BackendIsForeground(PyObject*,PyObject* args){PyObject*o=nullptr;if(!PyArg_ParseTuple(args,"O",&o))return nullptr;auto*b=GetBackend(o);if(!b)return nullptr;return PyBool_FromLong(kavio_ui_backend_is_foreground(b->backend));}

PyMethodDef kMethods[]={
    {"widget_create",PyCFunction_CAST(WidgetCreate),METH_VARARGS|METH_KEYWORDS,nullptr},
    {"widget_add",WidgetAdd,METH_VARARGS,nullptr},
    {"widget_set_style",PyCFunction_CAST(WidgetSetStyle),METH_VARARGS|METH_KEYWORDS,nullptr},
    {"widget_set_text",WidgetSetText,METH_VARARGS,nullptr},
    {"widget_set_source",WidgetSetSource,METH_VARARGS,nullptr},
    {"widget_set_buffer",WidgetSetBuffer,METH_VARARGS,nullptr},
    {"widget_set_range",WidgetSetRange,METH_VARARGS,nullptr},
    {"widget_set_value",WidgetSetValue,METH_VARARGS,nullptr},
    {"widget_get_value",WidgetGetValue,METH_VARARGS,nullptr},
    {"widget_set_checked",WidgetSetChecked,METH_VARARGS,nullptr},
    {"widget_get_checked",WidgetGetChecked,METH_VARARGS,nullptr},
    {"widget_set_click_callback",WidgetSetClick,METH_VARARGS,nullptr},
    {"widget_set_value_callback",WidgetSetValueCallback,METH_VARARGS,nullptr},
    {"widget_set_checked_callback",WidgetSetCheckedCallback,METH_VARARGS,nullptr},
    {"backend_create",BackendCreate,METH_NOARGS,nullptr},
    {"backend_initialize",BackendInitialize,METH_VARARGS,nullptr},
    {"backend_shutdown",BackendShutdown,METH_VARARGS,nullptr},
    {"backend_process",BackendProcess,METH_VARARGS,nullptr},
    {"backend_set_content",BackendSetContent,METH_VARARGS,nullptr},
    {"backend_clear_content",BackendClear,METH_VARARGS,nullptr},
    {"backend_is_ready",BackendIsReady,METH_VARARGS,nullptr},
    {"backend_is_foreground",BackendIsForeground,METH_VARARGS,nullptr},
    {nullptr,nullptr,0,nullptr}
};
PyModuleDef kModule={PyModuleDef_HEAD_INIT,"_ui_native","Kavio Python Widget/LVGL binding.",-1,kMethods,nullptr,nullptr,nullptr,nullptr};
}

PyMODINIT_FUNC PyInit__ui_native(void)
{
    PyObject* module=PyModule_Create(&kModule);if(!module)return nullptr;
    g_error=PyErr_NewException("kavio.ui.Error",PyExc_RuntimeError,nullptr);if(!g_error){Py_DECREF(module);return nullptr;}
    Py_INCREF(g_error);PyModule_AddObject(module,"Error",g_error);
    PyModule_AddIntConstant(module,"PAGE",KAVIO_WIDGET_PAGE);
    PyModule_AddIntConstant(module,"COLUMN",KAVIO_WIDGET_COLUMN);
    PyModule_AddIntConstant(module,"ROW",KAVIO_WIDGET_ROW);
    PyModule_AddIntConstant(module,"LABEL",KAVIO_WIDGET_LABEL);
    PyModule_AddIntConstant(module,"BUTTON",KAVIO_WIDGET_BUTTON);
    PyModule_AddIntConstant(module,"IMAGE",KAVIO_WIDGET_IMAGE);
    PyModule_AddIntConstant(module,"SLIDER",KAVIO_WIDGET_SLIDER);
    PyModule_AddIntConstant(module,"SWITCH",KAVIO_WIDGET_SWITCH);
    return module;
}
