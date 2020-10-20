/*
 * Copyright (C) 2020 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumquickcore.h"

#include "gumffi.h"
#include "gumquickinterceptor.h"
#include "gumquickmacros.h"
#include "gumquickscript-java.h"
#include "gumquickscript-objc.h"
#include "gumquickscript-priv.h"
#include "gumquickstalker.h"
#include "gumsourcemap.h"

#include <ffi.h>
#ifdef HAVE_PTRAUTH
# include <ptrauth.h>
#endif

#define GUM_QUICK_FFI_FUNCTION_PARAMS_EMPTY { NULL, }

typedef struct _GumQuickFlushCallback GumQuickFlushCallback;
typedef struct _GumQuickFFIFunctionParams GumQuickFFIFunctionParams;
typedef guint8 GumQuickSchedulingBehavior;
typedef guint8 GumQuickExceptionsBehavior;
typedef guint8 GumQuickCodeTraps;
typedef guint8 GumQuickReturnValueShape;
typedef struct _GumQuickFFIFunction GumQuickFFIFunction;
typedef struct _GumQuickNativeCallback GumQuickNativeCallback;

struct _GumQuickFlushCallback
{
  GumQuickFlushNotify func;
  GumQuickScript * script;
};

struct _GumQuickWeakRef
{
  guint id;
  JSValue callback;

  GumQuickCore * core;
};

struct _GumQuickScheduledCallback
{
  gint id;
  gboolean repeat;
  JSValue func;
  GSource * source;

  GumQuickCore * core;
};

struct _GumQuickExceptionSink
{
  JSValue callback;
  GumQuickCore * core;
};

struct _GumQuickMessageSink
{
  JSValue callback;
  GumQuickCore * core;
};

struct _GumQuickFFIFunctionParams
{
  GCallback implementation;
  JSValueConst return_type;
  JSValueConst argument_types;
  const gchar * abi_name;
  GumQuickSchedulingBehavior scheduling;
  GumQuickExceptionsBehavior exceptions;
  GumQuickCodeTraps traps;
  GumQuickReturnValueShape return_shape;

  JSContext * ctx;
};

enum _GumQuickSchedulingBehavior
{
  GUM_QUICK_SCHEDULING_COOPERATIVE,
  GUM_QUICK_SCHEDULING_EXCLUSIVE
};

enum _GumQuickExceptionsBehavior
{
  GUM_QUICK_EXCEPTIONS_STEAL,
  GUM_QUICK_EXCEPTIONS_PROPAGATE
};

enum _GumQuickCodeTraps
{
  GUM_QUICK_CODE_TRAPS_DEFAULT,
  GUM_QUICK_CODE_TRAPS_ALL
};

enum _GumQuickReturnValueShape
{
  GUM_QUICK_RETURN_PLAIN,
  GUM_QUICK_RETURN_DETAILED
};

struct _GumQuickFFIFunction
{
  GumQuickNativePointer native_pointer;

  GCallback implementation;
  GumQuickSchedulingBehavior scheduling;
  GumQuickExceptionsBehavior exceptions;
  GumQuickCodeTraps traps;
  GumQuickReturnValueShape return_shape;
  ffi_cif cif;
  ffi_type ** atypes;
  gsize arglist_size;
  gboolean is_variadic;
  guint nargs_fixed;
  ffi_abi abi;
  GSList * data;
};

struct _GumQuickNativeCallback
{
  GumQuickNativePointer native_pointer;

  JSValue func;
  ffi_closure * closure;
  ffi_cif cif;
  ffi_type ** atypes;
  GSList * data;

  GumQuickCore * core;
};

static void gum_quick_flush_callback_free (GumQuickFlushCallback * self);
static gboolean gum_quick_flush_callback_notify (GumQuickFlushCallback * self);

GUMJS_DECLARE_FUNCTION (gumjs_set_timeout)
GUMJS_DECLARE_FUNCTION (gumjs_set_interval)
GUMJS_DECLARE_FUNCTION (gumjs_clear_timer)
GUMJS_DECLARE_FUNCTION (gumjs_gc)
GUMJS_DECLARE_FUNCTION (gumjs_send)
GUMJS_DECLARE_FUNCTION (gumjs_set_unhandled_exception_callback)
GUMJS_DECLARE_FUNCTION (gumjs_set_incoming_message_callback)
GUMJS_DECLARE_FUNCTION (gumjs_wait_for_event)

GUMJS_DECLARE_GETTER (gumjs_frida_get_heap_size)
GUMJS_DECLARE_GETTER (gumjs_frida_get_source_map)
GUMJS_DECLARE_GETTER (gumjs_frida_objc_get_source_map)
GUMJS_DECLARE_GETTER (gumjs_frida_java_get_source_map)
GUMJS_DECLARE_FUNCTION (gumjs_frida_objc_load)
GUMJS_DECLARE_FUNCTION (gumjs_frida_java_load)

GUMJS_DECLARE_GETTER (gumjs_script_get_file_name)
GUMJS_DECLARE_GETTER (gumjs_script_get_source_map)
GUMJS_DECLARE_FUNCTION (gumjs_script_next_tick)
GUMJS_DECLARE_FUNCTION (gumjs_script_pin)
GUMJS_DECLARE_FUNCTION (gumjs_script_unpin)
GUMJS_DECLARE_FUNCTION (gumjs_script_set_global_access_handler)
static JSValue gum_quick_core_on_global_get (JSContext * ctx, JSAtom name,
    void * opaque);

GUMJS_DECLARE_FUNCTION (gumjs_weak_ref_bind)
GUMJS_DECLARE_FUNCTION (gumjs_weak_ref_unbind)

GUMJS_DECLARE_FINALIZER (gumjs_weak_ref_finalize)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_int64_construct)
GUMJS_DECLARE_FINALIZER (gumjs_int64_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_int64_add)
GUMJS_DECLARE_FUNCTION (gumjs_int64_sub)
GUMJS_DECLARE_FUNCTION (gumjs_int64_and)
GUMJS_DECLARE_FUNCTION (gumjs_int64_or)
GUMJS_DECLARE_FUNCTION (gumjs_int64_xor)
GUMJS_DECLARE_FUNCTION (gumjs_int64_shr)
GUMJS_DECLARE_FUNCTION (gumjs_int64_shl)
GUMJS_DECLARE_FUNCTION (gumjs_int64_not)
GUMJS_DECLARE_FUNCTION (gumjs_int64_compare)
GUMJS_DECLARE_FUNCTION (gumjs_int64_to_number)
GUMJS_DECLARE_FUNCTION (gumjs_int64_to_string)
GUMJS_DECLARE_FUNCTION (gumjs_int64_to_json)
GUMJS_DECLARE_FUNCTION (gumjs_int64_value_of)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_uint64_construct)
GUMJS_DECLARE_FINALIZER (gumjs_uint64_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_add)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_sub)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_and)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_or)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_xor)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_shr)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_shl)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_not)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_compare)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_to_number)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_to_string)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_to_json)
GUMJS_DECLARE_FUNCTION (gumjs_uint64_value_of)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_pointer_construct)
GUMJS_DECLARE_FINALIZER (gumjs_native_pointer_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_is_null)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_add)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_sub)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_and)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_or)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_xor)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_shr)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_shl)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_not)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_sign)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_strip)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_blend)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_compare)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_int32)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_uint32)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_string)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_json)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_match_pattern)

GUMJS_DECLARE_FUNCTION (gumjs_array_buffer_wrap)
GUMJS_DECLARE_FUNCTION (gumjs_array_buffer_unwrap)

GUMJS_DECLARE_FINALIZER (gumjs_native_resource_finalize)

GUMJS_DECLARE_FINALIZER (gumjs_kernel_resource_finalize)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_function_construct)
GUMJS_DECLARE_FINALIZER (gumjs_native_function_finalize)
GUMJS_DECLARE_CALL_HANDLER (gumjs_native_function_invoke)
GUMJS_DECLARE_FUNCTION (gumjs_native_function_call)
GUMJS_DECLARE_FUNCTION (gumjs_native_function_apply)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_system_function_construct)
GUMJS_DECLARE_FINALIZER (gumjs_system_function_finalize)
GUMJS_DECLARE_CALL_HANDLER (gumjs_system_function_invoke)
GUMJS_DECLARE_FUNCTION (gumjs_system_function_call)
GUMJS_DECLARE_FUNCTION (gumjs_system_function_apply)

static GumQuickFFIFunction * gumjs_ffi_function_new (JSContext * ctx,
    const GumQuickFFIFunctionParams * params, GumQuickCore * core);
static void gum_quick_ffi_function_finalize (GumQuickFFIFunction * func);
static JSValue gum_quick_ffi_function_invoke (GumQuickFFIFunction * self,
    JSContext * ctx, GCallback implementation, guint argc, JSValueConst * argv,
    GumQuickCore * core);
static JSValue gumjs_ffi_function_invoke (JSContext * ctx,
    JSValueConst func_obj, JSClassID klass, GumQuickArgs * args,
    GumQuickCore * core);
static JSValue gumjs_ffi_function_call (JSContext * ctx, JSValueConst func_obj,
    JSClassID klass, GumQuickArgs * args, GumQuickCore * core);
static JSValue gumjs_ffi_function_apply (JSContext * ctx, JSValueConst func_obj,
    JSClassID klass, GumQuickArgs * args, GumQuickCore * core);
static gboolean gumjs_ffi_function_get (JSContext * ctx, JSValueConst func_obj,
    JSValueConst receiver, JSClassID klass, GumQuickCore * core,
    GumQuickFFIFunction ** func, GCallback * implementation);

static gboolean gum_quick_ffi_function_params_init (
    GumQuickFFIFunctionParams * params, GumQuickReturnValueShape return_shape,
    GumQuickArgs * args);
static void gum_quick_ffi_function_params_destroy (
    GumQuickFFIFunctionParams * params);

static gboolean gum_quick_scheduling_behavior_get (JSContext * ctx,
    JSValueConst val, GumQuickSchedulingBehavior * behavior);
static gboolean gum_quick_exceptions_behavior_get (JSContext * ctx,
    JSValueConst val, GumQuickExceptionsBehavior * behavior);
static gboolean gum_quick_code_traps_get (JSContext * ctx, JSValueConst val,
    GumQuickCodeTraps * traps);

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_callback_construct)
GUMJS_DECLARE_FINALIZER (gumjs_native_callback_finalize)
static void gum_quick_native_callback_finalize (GumQuickNativeCallback * func);
static void gum_quick_native_callback_invoke (ffi_cif * cif,
    void * return_value, void ** args, void * user_data);

GUMJS_DECLARE_FINALIZER (gumjs_cpu_context_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_cpu_context_to_json)
static JSValue gumjs_cpu_context_set_register (GumQuickCpuContext * self,
    JSContext * ctx, JSValueConst val, gpointer * reg);

static JSValue gumjs_source_map_new (const gchar * json, GumQuickCore * core);
GUMJS_DECLARE_CONSTRUCTOR (gumjs_source_map_construct)
GUMJS_DECLARE_FINALIZER (gumjs_source_map_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_source_map_resolve)

static GumQuickWeakRef * gum_quick_weak_ref_new (guint id, JSValue callback,
    GumQuickCore * core);
static void gum_quick_weak_ref_clear (GumQuickWeakRef * ref);

static JSValue gum_quick_core_schedule_callback (GumQuickCore * self,
    GumQuickArgs * args, gboolean repeat);
static GumQuickScheduledCallback * gum_quick_core_try_steal_scheduled_callback (
    GumQuickCore * self, gint id);

static GumQuickScheduledCallback * gum_scheduled_callback_new (guint id,
    JSValueConst func, gboolean repeat, GSource * source, GumQuickCore * core);
static void gum_scheduled_callback_free (GumQuickScheduledCallback * callback);
static gboolean gum_scheduled_callback_invoke (
    GumQuickScheduledCallback * self);

static GumQuickExceptionSink * gum_quick_exception_sink_new (
    JSValueConst callback, GumQuickCore * core);
static void gum_quick_exception_sink_free (GumQuickExceptionSink * sink);
static void gum_quick_exception_sink_handle_exception (
    GumQuickExceptionSink * self, JSValueConst exception);

static GumQuickMessageSink * gum_quick_message_sink_new (JSValueConst callback,
    GumQuickCore * core);
static void gum_quick_message_sink_free (GumQuickMessageSink * sink);
static void gum_quick_message_sink_post (GumQuickMessageSink * self,
    const gchar * message, GBytes * data, GumQuickScope * scope);

static gboolean gum_quick_ffi_type_get (JSContext * ctx, JSValueConst val,
    GumQuickCore * core, ffi_type ** type, GSList ** data);
static gboolean gum_quick_ffi_abi_get (JSContext * ctx, const gchar * name,
    ffi_abi * abi);
static gboolean gum_quick_value_to_ffi (JSContext * ctx, JSValueConst sval,
    const ffi_type * type, GumQuickCore * core, GumFFIValue * val);
static JSValue gum_quick_value_from_ffi (JSContext * ctx,
    const GumFFIValue * val, const ffi_type * type, GumQuickCore * core);

static void gum_quick_core_setup_atoms (GumQuickCore * self);
static void gum_quick_core_teardown_atoms (GumQuickCore * self);

static const JSCFunctionListEntry gumjs_root_entries[] =
{
  JS_CFUNC_DEF ("_setTimeout", 0, gumjs_set_timeout),
  JS_CFUNC_DEF ("_setInterval", 0, gumjs_set_interval),
  JS_CFUNC_DEF ("clearTimeout", 1, gumjs_clear_timer),
  JS_CFUNC_DEF ("clearInterval", 1, gumjs_clear_timer),
  JS_CFUNC_DEF ("gc", 0, gumjs_gc),
  JS_CFUNC_DEF ("_send", 0, gumjs_send),
  JS_CFUNC_DEF ("_setUnhandledExceptionCallback", 0,
      gumjs_set_unhandled_exception_callback),
  JS_CFUNC_DEF ("_setIncomingMessageCallback", 0,
      gumjs_set_incoming_message_callback),
  JS_CFUNC_DEF ("_waitForEvent", 0, gumjs_wait_for_event),
};

static const JSCFunctionListEntry gumjs_frida_entries[] =
{
  JS_PROP_STRING_DEF ("version", FRIDA_VERSION, JS_PROP_C_W_E),
  JS_CGETSET_DEF ("heapSize", gumjs_frida_get_heap_size, NULL),
  JS_CGETSET_DEF ("sourceMap", gumjs_frida_get_source_map, NULL),
  JS_CGETSET_DEF ("_objcSourceMap", gumjs_frida_objc_get_source_map, NULL),
  JS_CGETSET_DEF ("_javaSourceMap", gumjs_frida_java_get_source_map, NULL),
  JS_CFUNC_DEF ("_loadObjC", 0, gumjs_frida_objc_load),
  JS_CFUNC_DEF ("_loadJava", 0, gumjs_frida_java_load),
};

static const JSCFunctionListEntry gumjs_script_entries[] =
{
  JS_PROP_STRING_DEF ("runtime", "QJS", JS_PROP_C_W_E),
  JS_CGETSET_DEF ("fileName", gumjs_script_get_file_name, NULL),
  JS_CGETSET_DEF ("sourceMap", gumjs_script_get_source_map, NULL),
  JS_CFUNC_DEF ("_nextTick", 0, gumjs_script_next_tick),
  JS_CFUNC_DEF ("pin", 0, gumjs_script_pin),
  JS_CFUNC_DEF ("unpin", 0, gumjs_script_unpin),
  JS_CFUNC_DEF ("setGlobalAccessHandler", 1,
      gumjs_script_set_global_access_handler),
};

static const JSCFunctionListEntry gumjs_weak_ref_module_entries[] =
{
  JS_CFUNC_DEF ("bind", 0, gumjs_weak_ref_bind),
  JS_CFUNC_DEF ("unbind", 0, gumjs_weak_ref_unbind),
};

static const JSClassDef gumjs_weak_ref_def =
{
  .class_name = "WeakRef",
  .finalizer = gumjs_weak_ref_finalize,
};

static const JSClassDef gumjs_int64_def =
{
  .class_name = "Int64",
  .finalizer = gumjs_int64_finalize,
};

static const JSCFunctionListEntry gumjs_int64_entries[] =
{
  JS_CFUNC_DEF ("add", 0, gumjs_int64_add),
  JS_CFUNC_DEF ("sub", 0, gumjs_int64_sub),
  JS_CFUNC_DEF ("and", 0, gumjs_int64_and),
  JS_CFUNC_DEF ("or", 0, gumjs_int64_or),
  JS_CFUNC_DEF ("xor", 0, gumjs_int64_xor),
  JS_CFUNC_DEF ("shr", 0, gumjs_int64_shr),
  JS_CFUNC_DEF ("shl", 0, gumjs_int64_shl),
  JS_CFUNC_DEF ("not", 0, gumjs_int64_not),
  JS_CFUNC_DEF ("compare", 0, gumjs_int64_compare),
  JS_CFUNC_DEF ("toNumber", 0, gumjs_int64_to_number),
  JS_CFUNC_DEF ("toString", 0, gumjs_int64_to_string),
  JS_CFUNC_DEF ("toJSON", 0, gumjs_int64_to_json),
  JS_CFUNC_DEF ("valueOf", 0, gumjs_int64_value_of),
};

static const JSClassDef gumjs_uint64_def =
{
  .class_name = "UInt64",
  .finalizer = gumjs_uint64_finalize,
};

static const JSCFunctionListEntry gumjs_uint64_entries[] =
{
  JS_CFUNC_DEF ("add", 0, gumjs_uint64_add),
  JS_CFUNC_DEF ("sub", 0, gumjs_uint64_sub),
  JS_CFUNC_DEF ("and", 0, gumjs_uint64_and),
  JS_CFUNC_DEF ("or", 0, gumjs_uint64_or),
  JS_CFUNC_DEF ("xor", 0, gumjs_uint64_xor),
  JS_CFUNC_DEF ("shr", 0, gumjs_uint64_shr),
  JS_CFUNC_DEF ("shl", 0, gumjs_uint64_shl),
  JS_CFUNC_DEF ("not", 0, gumjs_uint64_not),
  JS_CFUNC_DEF ("compare", 0, gumjs_uint64_compare),
  JS_CFUNC_DEF ("toNumber", 0, gumjs_uint64_to_number),
  JS_CFUNC_DEF ("toString", 0, gumjs_uint64_to_string),
  JS_CFUNC_DEF ("toJSON", 0, gumjs_uint64_to_json),
  JS_CFUNC_DEF ("valueOf", 0, gumjs_uint64_value_of),
};

static const JSClassDef gumjs_native_pointer_def =
{
  .class_name = "NativePointer",
  .finalizer = gumjs_native_pointer_finalize,
};

static const JSCFunctionListEntry gumjs_native_pointer_entries[] =
{
  JS_CFUNC_DEF ("isNull", 0, gumjs_native_pointer_is_null),
  JS_CFUNC_DEF ("add", 0, gumjs_native_pointer_add),
  JS_CFUNC_DEF ("sub", 0, gumjs_native_pointer_sub),
  JS_CFUNC_DEF ("and", 0, gumjs_native_pointer_and),
  JS_CFUNC_DEF ("or", 0, gumjs_native_pointer_or),
  JS_CFUNC_DEF ("xor", 0, gumjs_native_pointer_xor),
  JS_CFUNC_DEF ("shr", 0, gumjs_native_pointer_shr),
  JS_CFUNC_DEF ("shl", 0, gumjs_native_pointer_shl),
  JS_CFUNC_DEF ("not", 0, gumjs_native_pointer_not),
  JS_CFUNC_DEF ("sign", 0, gumjs_native_pointer_sign),
  JS_CFUNC_DEF ("strip", 0, gumjs_native_pointer_strip),
  JS_CFUNC_DEF ("blend", 0, gumjs_native_pointer_blend),
  JS_CFUNC_DEF ("compare", 0, gumjs_native_pointer_compare),
  JS_CFUNC_DEF ("toInt32", 0, gumjs_native_pointer_to_int32),
  JS_CFUNC_DEF ("toUInt32", 0, gumjs_native_pointer_to_uint32),
  JS_CFUNC_DEF ("toString", 0, gumjs_native_pointer_to_string),
  JS_CFUNC_DEF ("toJSON", 0, gumjs_native_pointer_to_json),
  JS_CFUNC_DEF ("toMatchPattern", 0,
      gumjs_native_pointer_to_match_pattern),
};

static const JSCFunctionListEntry gumjs_array_buffer_class_entries[] =
{
  JS_CFUNC_DEF ("wrap", 0, gumjs_array_buffer_wrap),
};

static const JSCFunctionListEntry gumjs_array_buffer_instance_entries[] =
{
  JS_CFUNC_DEF ("unwrap", 0, gumjs_array_buffer_unwrap),
};

static const JSClassDef gumjs_native_resource_def =
{
  .class_name = "NativeResource",
  .finalizer = gumjs_native_resource_finalize,
};

static const JSClassDef gumjs_kernel_resource_def =
{
  .class_name = "KernelResource",
  .finalizer = gumjs_kernel_resource_finalize,
};

static const JSClassDef gumjs_native_function_def =
{
  .class_name = "NativeFunction",
  .finalizer = gumjs_native_function_finalize,
  .call = gumjs_native_function_invoke,
};

static const JSCFunctionListEntry gumjs_native_function_entries[] =
{
  JS_CFUNC_DEF ("call", 0, gumjs_native_function_call),
  JS_CFUNC_DEF ("apply", 2, gumjs_native_function_apply),
};

static const JSClassDef gumjs_system_function_def =
{
  .class_name = "SystemFunction",
  .finalizer = gumjs_system_function_finalize,
  .call = gumjs_system_function_invoke,
};

static const JSCFunctionListEntry gumjs_system_function_entries[] =
{
  JS_CFUNC_DEF ("call", 0, gumjs_system_function_call),
  JS_CFUNC_DEF ("apply", 2, gumjs_system_function_apply),
};

static const JSClassDef gumjs_native_callback_def =
{
  .class_name = "NativeCallback",
  .finalizer = gumjs_native_callback_finalize,
};

static const JSClassDef gumjs_cpu_context_def =
{
  .class_name = "CpuContext",
  .finalizer = gumjs_cpu_context_finalize,
};

#define GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED(A, R) \
    GUMJS_DEFINE_GETTER (gumjs_cpu_context_get_##A) \
    { \
      GumQuickCpuContext * self; \
      \
      if (!_gum_quick_cpu_context_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      \
      return _gum_quick_native_pointer_new (ctx, \
          GSIZE_TO_POINTER (self->handle->R), core); \
    } \
    \
    GUMJS_DEFINE_SETTER (gumjs_cpu_context_set_##A) \
    { \
      GumQuickCpuContext * self; \
      \
      if (!_gum_quick_cpu_context_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      \
      return gumjs_cpu_context_set_register (self, ctx, val, \
          (gpointer *) &self->handle->R); \
    }
#define GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR(R) \
    GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (R, R)

#define GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED(A, R) \
    JS_CGETSET_DEF (G_STRINGIFY (A), gumjs_cpu_context_get_##R, \
        gumjs_cpu_context_set_##R)
#define GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR(R) \
    GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (R, R)

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (eax)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ecx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (edx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ebx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (esp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ebp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (esi)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (edi)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (eip)
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rax)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rcx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rdx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rbx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rsp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rbp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rsi)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rdi)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r8)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r9)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r10)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r11)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r12)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r13)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r14)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r15)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rip)
#elif defined (HAVE_ARM)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (pc)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (sp)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r0, r[0])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r1, r[1])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r2, r[2])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r3, r[3])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r4, r[4])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r5, r[5])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r6, r[6])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r7, r[7])

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r8)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r9)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r10)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r11)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r12)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (lr)
#elif defined (HAVE_ARM64)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (pc)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (sp)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x0, x[0])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x1, x[1])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x2, x[2])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x3, x[3])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x4, x[4])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x5, x[5])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x6, x[6])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x7, x[7])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x8, x[8])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x9, x[9])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x10, x[10])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x11, x[11])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x12, x[12])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x13, x[13])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x14, x[14])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x15, x[15])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x16, x[16])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x17, x[17])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x18, x[18])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x19, x[19])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x20, x[20])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x21, x[21])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x22, x[22])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x23, x[23])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x24, x[24])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x25, x[25])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x26, x[26])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x27, x[27])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x28, x[28])

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (fp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (lr)
#elif defined (HAVE_MIPS)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (pc)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (gp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (sp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (fp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ra)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (hi)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (lo)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (at)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (v0)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (v1)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (a0)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (a1)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (a2)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (a3)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t0)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t1)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t2)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t3)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t4)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t5)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t6)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t7)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t8)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (t9)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s0)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s1)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s2)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s3)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s4)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s5)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s6)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (s7)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (k0)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (k1)
#endif

static const JSCFunctionListEntry gumjs_cpu_context_entries[] =
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (pc, eip),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (sp, esp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (eax),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ecx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (edx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ebx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (esp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ebp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (esi),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (edi),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (eip),
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (pc, rip),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (sp, rsp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rax),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rcx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rdx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rbx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rsp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rbp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rsi),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rdi),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r9),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r10),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r11),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r12),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r13),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r14),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r15),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rip),
#elif defined (HAVE_ARM)
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (pc),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (sp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r7),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r9),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r10),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r11),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r12),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (lr),
#elif defined (HAVE_ARM64)
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (pc),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (sp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x7),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x9),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x10),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x11),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x12),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x13),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x14),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x15),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x16),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x17),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x18),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x19),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x20),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x21),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x22),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x23),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x24),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x25),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x26),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x27),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x28),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (fp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (lr),
#elif defined (HAVE_MIPS)
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (pc),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (gp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (sp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (fp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ra),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (hi),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (lo),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (at),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (v0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (v1),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (a0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (a1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (a2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (a3),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t7),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (t9),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (s7),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (k0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (k1),
#endif

  JS_CFUNC_DEF ("toJSON", 0, gumjs_cpu_context_to_json),
};

static const JSClassDef gumjs_source_map_def =
{
  .class_name = "SourceMap",
  .finalizer = gumjs_source_map_finalize,
};

static const JSCFunctionListEntry gumjs_source_map_entries[] =
{
  JS_CFUNC_DEF ("_resolve", 0, gumjs_source_map_resolve),
};

void
_gum_quick_core_init (GumQuickCore * self,
                      GumQuickScript * script,
                      JSContext * ctx,
                      JSValue ns,
                      GRecMutex * mutex,
                      const gchar * runtime_source_map,
                      GumQuickInterceptor * interceptor,
                      GumQuickStalker * stalker,
                      GumQuickMessageEmitter message_emitter,
                      GumScriptScheduler * scheduler)
{
  JSRuntime * rt;
  JSValue obj, proto, ctor, uint64_proto, global_obj;

  rt = JS_GetRuntime (ctx);

  g_object_get (script, "backend", &self->backend, NULL);
  g_object_unref (self->backend);

  self->script = script;
  self->runtime_source_map = runtime_source_map;
  self->interceptor = interceptor;
  self->stalker = stalker;
  self->message_emitter = message_emitter;
  self->scheduler = scheduler;
  self->exceptor = gum_exceptor_obtain ();
  self->rt = rt;
  self->ctx = ctx;
  self->module_data =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->current_scope = NULL;

  self->mutex = mutex;
  self->usage_count = 0;
  self->mutex_depth = 0;
  self->flush_notify = NULL;

  self->event_loop = g_main_loop_new (
      gum_script_scheduler_get_js_context (scheduler), FALSE);
  g_mutex_init (&self->event_mutex);
  g_cond_init (&self->event_cond);
  self->event_count = 0;
  self->event_source_available = TRUE;

  self->on_global_get = JS_NULL;
  self->global_receiver = JS_NULL;

  self->weak_refs = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gum_quick_weak_ref_clear);
  self->next_weak_ref_id = 1;

  self->scheduled_callbacks = g_hash_table_new (NULL, NULL);
  self->next_callback_id = 1;

  self->subclasses = g_hash_table_new (NULL, NULL);

  gum_quick_core_setup_atoms (self);

  JS_SetPropertyFunctionList (ctx, ns, gumjs_root_entries,
      G_N_ELEMENTS (gumjs_root_entries));

  obj = JS_NewObject (ctx);
  JS_SetPropertyFunctionList (ctx, obj, gumjs_frida_entries,
      G_N_ELEMENTS (gumjs_frida_entries));
  JS_DefinePropertyValueStr (ctx, ns, "Frida", obj, JS_PROP_C_W_E);

  obj = JS_NewObject (ctx);
  JS_SetPropertyFunctionList (ctx, obj, gumjs_script_entries,
      G_N_ELEMENTS (gumjs_script_entries));
  JS_DefinePropertyValueStr (ctx, ns, "Script", obj, JS_PROP_C_W_E);

  obj = JS_NewObject (ctx);
  JS_SetPropertyFunctionList (ctx, obj, gumjs_weak_ref_module_entries,
      G_N_ELEMENTS (gumjs_weak_ref_module_entries));
  JS_DefinePropertyValueStr (ctx, ns, "WeakRef", obj, JS_PROP_C_W_E);

  _gum_quick_create_class (ctx, &gumjs_weak_ref_def, self,
      &self->weak_ref_class, &proto);

  _gum_quick_create_class (ctx, &gumjs_int64_def, self, &self->int64_class,
      &proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_int64_construct,
      gumjs_int64_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_int64_entries,
      G_N_ELEMENTS (gumjs_int64_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_int64_def.class_name, ctor,
      JS_PROP_C_W_E);

  _gum_quick_create_class (ctx, &gumjs_uint64_def, self, &self->uint64_class,
      &proto);
  uint64_proto = proto;
  ctor = JS_NewCFunction2 (ctx, gumjs_uint64_construct,
      gumjs_uint64_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_uint64_entries,
      G_N_ELEMENTS (gumjs_uint64_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_uint64_def.class_name, ctor,
      JS_PROP_C_W_E);

  _gum_quick_create_class (ctx, &gumjs_native_pointer_def, self,
      &self->native_pointer_class, &proto);
  self->native_pointer_proto = JS_DupValue (ctx, proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_native_pointer_construct,
      gumjs_native_pointer_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_native_pointer_entries,
      G_N_ELEMENTS (gumjs_native_pointer_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_native_pointer_def.class_name, ctor,
      JS_PROP_C_W_E);

  global_obj = JS_GetGlobalObject (ctx);
  obj = JS_GetPropertyStr (ctx, global_obj, "ArrayBuffer");
  JS_SetPropertyFunctionList (ctx, obj, gumjs_array_buffer_class_entries,
      G_N_ELEMENTS (gumjs_array_buffer_class_entries));
  proto = JS_GetProperty (ctx, obj, GUM_QUICK_CORE_ATOM (self, prototype));
  JS_SetPropertyFunctionList (ctx, proto, gumjs_array_buffer_instance_entries,
      G_N_ELEMENTS (gumjs_array_buffer_instance_entries));
  JS_FreeValue (ctx, proto);
  JS_FreeValue (ctx, obj);
  JS_FreeValue (ctx, global_obj);

  _gum_quick_create_subclass (ctx, &gumjs_native_resource_def,
      self->native_pointer_class, self->native_pointer_proto, self,
      &self->native_resource_class, &proto);

  _gum_quick_create_subclass (ctx, &gumjs_kernel_resource_def,
      self->uint64_class, uint64_proto, self, &self->kernel_resource_class,
      &proto);

  _gum_quick_create_subclass (ctx, &gumjs_native_function_def,
      self->native_pointer_class, self->native_pointer_proto, self,
      &self->native_function_class, &proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_native_function_construct,
      gumjs_native_function_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_native_function_entries,
      G_N_ELEMENTS (gumjs_native_function_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_native_function_def.class_name,
      ctor, JS_PROP_C_W_E);

  _gum_quick_create_subclass (ctx, &gumjs_system_function_def,
      self->native_pointer_class, self->native_pointer_proto, self,
      &self->system_function_class, &proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_system_function_construct,
      gumjs_system_function_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_system_function_entries,
      G_N_ELEMENTS (gumjs_system_function_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_system_function_def.class_name,
      ctor, JS_PROP_C_W_E);

  _gum_quick_create_subclass (ctx, &gumjs_native_callback_def,
      self->native_pointer_class, self->native_pointer_proto, self,
      &self->native_callback_class, &proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_native_callback_construct,
      gumjs_native_callback_def.class_name, 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor (ctx, ctor, proto);
  JS_DefinePropertyValueStr (ctx, ns, gumjs_native_callback_def.class_name,
      ctor, JS_PROP_C_W_E);

  _gum_quick_create_class (ctx, &gumjs_cpu_context_def, self,
      &self->cpu_context_class, &proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_cpu_context_entries,
      G_N_ELEMENTS (gumjs_cpu_context_entries));

  _gum_quick_create_class (ctx, &gumjs_source_map_def, self,
      &self->source_map_class, &proto);
  ctor = JS_NewCFunction2 (ctx, gumjs_source_map_construct,
      gumjs_source_map_def.class_name, 0, JS_CFUNC_constructor, 0);
  self->source_map_ctor = JS_DupValue (ctx, ctor);
  JS_SetConstructor (ctx, ctor, proto);
  JS_SetPropertyFunctionList (ctx, proto, gumjs_source_map_entries,
      G_N_ELEMENTS (gumjs_source_map_entries));
  JS_DefinePropertyValueStr (ctx, ns, gumjs_source_map_def.class_name, ctor,
      JS_PROP_C_W_E);
}

gboolean
_gum_quick_core_flush (GumQuickCore * self,
                       GumQuickFlushNotify flush_notify)
{
  GHashTableIter iter;
  GumQuickScheduledCallback * callback;
  gboolean done;

  self->flush_notify = flush_notify;

  g_mutex_lock (&self->event_mutex);
  self->event_source_available = FALSE;
  g_cond_broadcast (&self->event_cond);
  g_mutex_unlock (&self->event_mutex);
  g_main_loop_quit (self->event_loop);

  if (self->usage_count > 1)
    return FALSE;

  g_hash_table_iter_init (&iter, self->scheduled_callbacks);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &callback))
  {
    _gum_quick_core_pin (self);
    g_source_destroy (callback->source);
  }
  g_hash_table_remove_all (self->scheduled_callbacks);

  if (self->usage_count > 1)
    return FALSE;

  g_hash_table_remove_all (self->weak_refs);

  done = self->usage_count == 1;
  if (done)
    self->flush_notify = NULL;

  return done;
}

static void
gum_quick_core_notify_flushed (GumQuickCore * self,
                               GumQuickFlushNotify func)
{
  GumQuickFlushCallback * cb;
  GSource * source;

  cb = g_slice_new (GumQuickFlushCallback);
  cb->func = func;
  cb->script = g_object_ref (self->script);

  source = g_idle_source_new ();
  g_source_set_callback (source, (GSourceFunc) gum_quick_flush_callback_notify,
      cb, (GDestroyNotify) gum_quick_flush_callback_free);
  g_source_attach (source,
      gum_script_scheduler_get_js_context (self->scheduler));
  g_source_unref (source);
}

static void
gum_quick_flush_callback_free (GumQuickFlushCallback * self)
{
  g_object_unref (self->script);

  g_slice_free (GumQuickFlushCallback, self);
}

static gboolean
gum_quick_flush_callback_notify (GumQuickFlushCallback * self)
{
  self->func (self->script);
  return FALSE;
}

void
_gum_quick_core_dispose (GumQuickCore * self)
{
  JSContext * ctx = self->ctx;

  JS_SetGlobalAccessFunctions (ctx, NULL);

  JS_FreeValue (ctx, self->on_global_get);
  JS_FreeValue (ctx, self->global_receiver);
  self->on_global_get = JS_NULL;
  self->global_receiver = JS_NULL;

  g_clear_pointer (&self->unhandled_exception_sink,
      gum_quick_exception_sink_free);

  g_clear_pointer (&self->incoming_message_sink, gum_quick_message_sink_free);

  g_clear_pointer (&self->exceptor, g_object_unref);

  JS_FreeValue (ctx, self->source_map_ctor);
  JS_FreeValue (ctx, self->native_pointer_proto);

  gum_quick_core_teardown_atoms (self);
}

void
_gum_quick_core_finalize (GumQuickCore * self)
{
  g_hash_table_unref (self->subclasses);
  self->subclasses = NULL;

  g_hash_table_unref (self->scheduled_callbacks);
  self->scheduled_callbacks = NULL;

  g_hash_table_unref (self->weak_refs);
  self->weak_refs = NULL;

  g_main_loop_unref (self->event_loop);
  self->event_loop = NULL;
  g_mutex_clear (&self->event_mutex);
  g_cond_clear (&self->event_cond);

  g_assert (self->current_scope == NULL);
  self->ctx = NULL;

  g_hash_table_unref (self->module_data);
  self->module_data = NULL;
}

void
_gum_quick_core_pin (GumQuickCore * self)
{
  self->usage_count++;
}

void
_gum_quick_core_unpin (GumQuickCore * self)
{
  self->usage_count--;
}

void
_gum_quick_core_post (GumQuickCore * self,
                      const gchar * message,
                      GBytes * data)
{
  gboolean delivered = FALSE;
  GumQuickScope scope;

  _gum_quick_scope_enter (&scope, self);

  if (self->incoming_message_sink != NULL)
  {
    gum_quick_message_sink_post (self->incoming_message_sink, message, data,
        &scope);
    delivered = TRUE;
  }

  _gum_quick_scope_leave (&scope);

  if (delivered)
  {
    g_mutex_lock (&self->event_mutex);
    self->event_count++;
    g_cond_broadcast (&self->event_cond);
    g_mutex_unlock (&self->event_mutex);

    g_main_loop_quit (self->event_loop);
  }
  else
  {
    g_bytes_unref (data);
  }
}

void
_gum_quick_core_push_job (GumQuickCore * self,
                          GumScriptJobFunc job_func,
                          gpointer data,
                          GDestroyNotify data_destroy)
{
  gum_script_scheduler_push_job_on_thread_pool (self->scheduler, job_func,
      data, data_destroy);
}

void
_gum_quick_core_store_module_data (GumQuickCore * self,
                                   const gchar * key,
                                   gpointer value)
{
  g_hash_table_insert (self->module_data, g_strdup (key), value);
}

gpointer
_gum_quick_core_load_module_data (GumQuickCore * self,
                                  const gchar * key)
{
  return g_hash_table_lookup (self->module_data, key);
}

void
_gum_quick_scope_enter (GumQuickScope * self,
                        GumQuickCore * core)
{
  self->core = core;

  g_rec_mutex_lock (core->mutex);

  gum_interceptor_begin_transaction (core->interceptor->interceptor);

  _gum_quick_core_pin (core);
  core->mutex_depth++;

  if (core->mutex_depth == 1)
  {
    g_assert (core->current_scope == NULL);
    core->current_scope = self;

    JS_Enter (core->rt);
  }

  g_queue_init (&self->tick_callbacks);
  g_queue_init (&self->scheduled_sources);

  self->pending_stalker_level = 0;
  self->pending_stalker_transformer = NULL;
  self->pending_stalker_sink = NULL;
}

void
_gum_quick_scope_suspend (GumQuickScope * self)
{
  GumQuickCore * core = self->core;
  guint i;

  gum_interceptor_end_transaction (core->interceptor->interceptor);

  JS_Suspend (core->rt, &self->thread_state);

  g_assert (core->current_scope != NULL);
  self->previous_scope = g_steal_pointer (&core->current_scope);

  self->previous_mutex_depth = core->mutex_depth;
  core->mutex_depth = 0;

  for (i = 0; i != self->previous_mutex_depth; i++)
    g_rec_mutex_unlock (core->mutex);
}

void
_gum_quick_scope_resume (GumQuickScope * self)
{
  GumQuickCore * core = self->core;
  guint i;

  for (i = 0; i != self->previous_mutex_depth; i++)
    g_rec_mutex_lock (core->mutex);

  g_assert (core->current_scope == NULL);
  core->current_scope = g_steal_pointer (&self->previous_scope);

  core->mutex_depth = self->previous_mutex_depth;
  self->previous_mutex_depth = 0;

  JS_Resume (core->rt, &self->thread_state);

  gum_interceptor_begin_transaction (core->interceptor->interceptor);
}

JSValue
_gum_quick_scope_call (GumQuickScope * self,
                       JSValueConst func_obj,
                       JSValueConst this_obj,
                       int argc,
                       JSValueConst * argv)
{
  JSValue result;
  GumQuickCore * core = self->core;
  JSContext * ctx = core->ctx;

  result = JS_Call (ctx, func_obj, this_obj, argc, argv);

  if (JS_IsException (result))
    _gum_quick_scope_catch_and_emit (self);

  return result;
}

gboolean
_gum_quick_scope_call_void (GumQuickScope * self,
                            JSValueConst func_obj,
                            JSValueConst this_obj,
                            int argc,
                            JSValueConst * argv)
{
  JSValue result;

  result = _gum_quick_scope_call (self, func_obj, this_obj, argc, argv);
  if (JS_IsException (result))
    return FALSE;

  JS_FreeValue (self->core->ctx, result);

  return TRUE;
}

void
_gum_quick_scope_catch_and_emit (GumQuickScope * self)
{
  GumQuickCore * core = self->core;
  JSContext * ctx = core->ctx;
  JSValue exception;

  exception = JS_GetException (ctx);
  if (JS_IsNull (exception))
    return;

  if (core->unhandled_exception_sink != NULL)
  {
    gum_quick_exception_sink_handle_exception (
        core->unhandled_exception_sink, exception);
  }

  JS_FreeValue (ctx, exception);
}

void
_gum_quick_scope_perform_pending_io (GumQuickScope * self)
{
  GumQuickCore * core = self->core;
  JSContext * ctx = core->ctx;
  gboolean io_performed;

  do
  {
    JSContext * pctx;
    JSValue * tick_callback;
    GSource * source;

    io_performed = FALSE;

    do
    {
      int res = JS_ExecutePendingJob (core->rt, &pctx);
      if (res == -1)
        _gum_quick_scope_catch_and_emit (self);
    }
    while (pctx != NULL);

    while ((tick_callback = g_queue_pop_head (&self->tick_callbacks)) != NULL)
    {
      _gum_quick_scope_call_void (self, *tick_callback, JS_UNDEFINED, 0, NULL);

      JS_FreeValue (ctx, *tick_callback);
      g_slice_free (JSValue, tick_callback);

      io_performed = TRUE;
    }

    while ((source = g_queue_pop_head (&self->scheduled_sources)) != NULL)
    {
      if (!g_source_is_destroyed (source))
      {
        g_source_attach (source,
            gum_script_scheduler_get_js_context (core->scheduler));
      }

      g_source_unref (source);

      io_performed = TRUE;
    }
  }
  while (io_performed);
}

void
_gum_quick_scope_leave (GumQuickScope * self)
{
  GumQuickCore * core = self->core;
  GumQuickFlushNotify pending_flush_notify = NULL;

  _gum_quick_scope_perform_pending_io (self);

  if (core->mutex_depth == 1)
  {
    JS_Leave (core->rt);

    core->current_scope = NULL;
  }

  core->mutex_depth--;
  _gum_quick_core_unpin (core);

  if (core->flush_notify != NULL && core->usage_count == 0)
  {
    pending_flush_notify = core->flush_notify;
    core->flush_notify = NULL;
  }

  gum_interceptor_end_transaction (self->core->interceptor->interceptor);

  g_rec_mutex_unlock (core->mutex);

  if (pending_flush_notify != NULL)
    gum_quick_core_notify_flushed (core, pending_flush_notify);

  _gum_quick_stalker_process_pending (core->stalker, self);
}

GUMJS_DEFINE_GETTER (gumjs_frida_get_heap_size)
{
  return JS_NewUint32 (ctx, gum_peek_private_memory_usage ());
}

GUMJS_DEFINE_GETTER (gumjs_frida_get_source_map)
{
  return gumjs_source_map_new (core->runtime_source_map, core);
}

GUMJS_DEFINE_GETTER (gumjs_frida_objc_get_source_map)
{
  return gumjs_source_map_new (gumjs_objc_source_map, core);
}

GUMJS_DEFINE_GETTER (gumjs_frida_java_get_source_map)
{
  return gumjs_source_map_new (gumjs_java_source_map, core);
}

GUMJS_DEFINE_FUNCTION (gumjs_frida_objc_load)
{
  gum_quick_bundle_load (gumjs_objc_modules, ctx);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_frida_java_load)
{
  gum_quick_bundle_load (gumjs_java_modules, ctx);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_GETTER (gumjs_script_get_file_name)
{
  JSValue result;
  gchar * name, * file_name;

  g_object_get (core->script, "name", &name, NULL);
  file_name = g_strconcat ("/", name, ".js", NULL);
  result = JS_NewString (ctx, file_name);
  g_free (file_name);
  g_free (name);

  return result;
}

GUMJS_DEFINE_GETTER (gumjs_script_get_source_map)
{
  JSValue result;
  gchar * source;
  GRegex * regex;
  GMatchInfo * match_info;

  g_object_get (core->script, "source", &source, NULL);

  if (source == NULL)
    return JS_NULL;

  regex = g_regex_new ("//[#@][ \\t]sourceMappingURL=[ \\t]*"
      "data:application/json;.*?base64,([^\\s'\"]*)[ \\t]*$",
      G_REGEX_MULTILINE, 0, NULL);
  g_regex_match (regex, source, 0, &match_info);
  if (g_match_info_matches (match_info))
  {
    gchar * data_encoded;
    gsize size;
    gchar * data;

    data_encoded = g_match_info_fetch (match_info, 1);

    data = (gchar *) g_base64_decode (data_encoded, &size);
    if (data != NULL && g_utf8_validate (data, size, NULL))
    {
      gchar * data_utf8;

      data_utf8 = g_strndup (data, size);
      result = gumjs_source_map_new (data_utf8, core);
      g_free (data_utf8);
    }
    else
    {
      result = JS_NULL;
    }
    g_free (data);

    g_free (data_encoded);
  }
  else
  {
    result = JS_NULL;
  }
  g_match_info_free (match_info);
  g_regex_unref (regex);

  g_free (source);

  return result;
}

GUMJS_DEFINE_FUNCTION (gumjs_script_next_tick)
{
  JSValue callback;

  if (!_gum_quick_args_parse (args, "F", &callback))
    return JS_EXCEPTION;

  JS_DupValue (ctx, callback);
  g_queue_push_tail (&core->current_scope->tick_callbacks,
      g_slice_dup (JSValue, &callback));

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_script_pin)
{
  _gum_quick_core_pin (core);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_script_unpin)
{
  _gum_quick_core_unpin (core);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_script_set_global_access_handler)
{
  JSValueConst * argv = args->elements;
  JSValue receiver, get;

  if (!JS_IsNull (argv[0]))
  {
    receiver = argv[0];
    if (!_gum_quick_args_parse (args, "F{get}", &get))
      return JS_EXCEPTION;
  }
  else
  {
    receiver = JS_NULL;
    get = JS_NULL;
  }

  if (JS_IsNull (receiver))
    JS_SetGlobalAccessFunctions (ctx, NULL);

  JS_FreeValue (ctx, core->on_global_get);
  JS_FreeValue (ctx, core->global_receiver);
  core->on_global_get = JS_NULL;
  core->global_receiver = JS_NULL;

  if (!JS_IsNull (receiver))
  {
    JSGlobalAccessFunctions funcs;

    core->on_global_get = JS_DupValue (ctx, get);
    core->global_receiver = JS_DupValue (ctx, receiver);

    funcs.get = gum_quick_core_on_global_get;
    funcs.opaque = core;
    JS_SetGlobalAccessFunctions (ctx, &funcs);
  }

  return JS_UNDEFINED;
}

static JSValue
gum_quick_core_on_global_get (JSContext * ctx,
                              JSAtom name,
                              void * opaque)
{
  GumQuickCore * self = opaque;
  JSValue result;
  JSValue name_val;

  name_val = JS_AtomToValue (ctx, name);

  result = _gum_quick_scope_call (self->current_scope, self->on_global_get,
      self->global_receiver, 1, &name_val);

  JS_FreeValue (ctx, name_val);

  return result;
}

GUMJS_DEFINE_FUNCTION (gumjs_weak_ref_bind)
{
  JSValue target, callback;
  gboolean target_is_valid;
  guint id;
  gchar prop_name[2 + 8 + 1];
  GumQuickWeakRef * ref;
  JSValue obj;

  if (!_gum_quick_args_parse (args, "VF", &target, &callback))
    return JS_EXCEPTION;

  target_is_valid = JS_IsObject (target);
  if (!target_is_valid)
    return _gum_quick_throw_literal (ctx, "expected a heap value");

  id = core->next_weak_ref_id++;

  ref = gum_quick_weak_ref_new (id, callback, core);
  g_hash_table_insert (core->weak_refs, GUINT_TO_POINTER (id), ref);

  obj = JS_NewObjectClass (ctx, core->weak_ref_class);
  JS_SetOpaque (obj, ref);
  JS_DefinePropertyValue (ctx, obj, GUM_QUICK_CORE_ATOM (core, resource),
      JS_DupValue (ctx, callback),
      JS_PROP_C_W_E);

  sprintf (prop_name, "$w%x", id);
  JS_DefinePropertyValueStr (ctx, target, prop_name, obj, 0);

  return JS_NewInt32 (ctx, id);
}

GUMJS_DEFINE_FUNCTION (gumjs_weak_ref_unbind)
{
  guint id;
  gboolean removed;

  if (!_gum_quick_args_parse (args, "u", &id))
    return JS_EXCEPTION;

  removed = !g_hash_table_remove (core->weak_refs, GUINT_TO_POINTER (id));

  return JS_NewBool (ctx, removed);
}

GUMJS_DEFINE_FINALIZER (gumjs_weak_ref_finalize)
{
  GumQuickWeakRef * r;

  r = JS_GetOpaque (val, core->weak_ref_class);

  if (r->core != NULL)
  {
    g_hash_table_remove (r->core->weak_refs, GUINT_TO_POINTER (r->id));
  }

  g_slice_free (GumQuickWeakRef, r);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_timeout)
{
  GumQuickCore * self = core;

  return gum_quick_core_schedule_callback (self, args, FALSE);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_interval)
{
  GumQuickCore * self = core;

  return gum_quick_core_schedule_callback (self, args, TRUE);
}

GUMJS_DEFINE_FUNCTION (gumjs_clear_timer)
{
  GumQuickCore * self = core;
  gint id;
  GumQuickScheduledCallback * callback;

  if (!JS_IsNumber (args->elements[0]))
    goto invalid_handle;

  if (!_gum_quick_args_parse (args, "i", &id))
    return JS_EXCEPTION;

  callback = gum_quick_core_try_steal_scheduled_callback (self, id);
  if (callback != NULL)
  {
    _gum_quick_core_pin (self);
    g_source_destroy (callback->source);
  }

  return JS_NewBool (ctx, callback != NULL);

invalid_handle:
  {
    return JS_NewBool (ctx, FALSE);
  }
}

GUMJS_DEFINE_FUNCTION (gumjs_gc)
{
  JS_RunGC (core->rt);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_send)
{
  GumQuickCore * self = core;
  GumInterceptor * interceptor = self->interceptor->interceptor;
  const char * message;
  GBytes * data;

  if (!_gum_quick_args_parse (args, "sB?", &message, &data))
    return JS_EXCEPTION;

  /*
   * Synchronize Interceptor state before sending the message. The application
   * might be waiting for an acknowledgement that APIs have been instrumented.
   *
   * This is very important for the RPC API.
   */
  gum_interceptor_end_transaction (interceptor);
  gum_interceptor_begin_transaction (interceptor);

  self->message_emitter (self->script, message, data);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_set_unhandled_exception_callback)
{
  GumQuickCore * self = core;
  JSValue callback;
  GumQuickExceptionSink * new_sink, * old_sink;

  if (!_gum_quick_args_parse (args, "F?", &callback))
    return JS_EXCEPTION;

  new_sink = !JS_IsNull (callback)
      ? gum_quick_exception_sink_new (callback, self)
      : NULL;

  old_sink = self->unhandled_exception_sink;
  self->unhandled_exception_sink = new_sink;

  if (old_sink != NULL)
    gum_quick_exception_sink_free (old_sink);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_set_incoming_message_callback)
{
  GumQuickCore * self = core;
  JSValue callback;
  GumQuickMessageSink * new_sink, * old_sink;

  if (!_gum_quick_args_parse (args, "F?", &callback))
    return JS_EXCEPTION;

  new_sink = !JS_IsNull (callback)
      ? gum_quick_message_sink_new (callback, self)
      : NULL;

  old_sink = self->incoming_message_sink;
  self->incoming_message_sink = new_sink;

  if (old_sink != NULL)
    gum_quick_message_sink_free (old_sink);

  return JS_UNDEFINED;
}

GUMJS_DEFINE_FUNCTION (gumjs_wait_for_event)
{
  GumQuickCore * self = core;
  GumQuickScope scope = GUM_QUICK_SCOPE_INIT (self);
  GMainContext * context;
  gboolean called_from_js_thread;
  guint start_count;
  gboolean event_source_available;

  _gum_quick_scope_perform_pending_io (self->current_scope);

  _gum_quick_scope_suspend (&scope);

  context = gum_script_scheduler_get_js_context (self->scheduler);
  called_from_js_thread = g_main_context_is_owner (context);

  g_mutex_lock (&self->event_mutex);

  start_count = self->event_count;
  while (self->event_count == start_count && self->event_source_available)
  {
    if (called_from_js_thread)
    {
      g_mutex_unlock (&self->event_mutex);
      g_main_loop_run (self->event_loop);
      g_mutex_lock (&self->event_mutex);
    }
    else
    {
      g_cond_wait (&self->event_cond, &self->event_mutex);
    }
  }

  event_source_available = self->event_source_available;

  g_mutex_unlock (&self->event_mutex);

  _gum_quick_scope_resume (&scope);

  if (!event_source_available)
    return _gum_quick_throw_literal (ctx, "script is unloading");

  return JS_UNDEFINED;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_int64_construct)
{
  JSValue wrapper;
  gint64 value;
  JSValue proto;
  GumQuickInt64 * i64;

  if (!_gum_quick_args_parse (args, "q~", &value))
    return JS_EXCEPTION;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->int64_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    return JS_EXCEPTION;

  i64 = g_slice_new (GumQuickInt64);
  i64->value = value;

  JS_SetOpaque (wrapper, i64);

  return wrapper;
}

GUMJS_DEFINE_FINALIZER (gumjs_int64_finalize)
{
  GumQuickInt64 * i;

  i = JS_GetOpaque (val, core->int64_class);
  if (i == NULL)
    return;

  g_slice_free (GumQuickInt64, i);
}

#define GUM_DEFINE_INT64_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_int64_##name) \
    { \
      GumQuickInt64 * self; \
      gint64 lhs, rhs, result; \
      \
      if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      lhs = self->value; \
      \
      if (!_gum_quick_args_parse (args, "q~", &rhs)) \
        return JS_EXCEPTION; \
      \
      result = lhs op rhs; \
      \
      return _gum_quick_int64_new (ctx, result, core); \
    }

GUM_DEFINE_INT64_OP_IMPL (add, +)
GUM_DEFINE_INT64_OP_IMPL (sub, -)
GUM_DEFINE_INT64_OP_IMPL (and, &)
GUM_DEFINE_INT64_OP_IMPL (or,  |)
GUM_DEFINE_INT64_OP_IMPL (xor, ^)
GUM_DEFINE_INT64_OP_IMPL (shr, >>)
GUM_DEFINE_INT64_OP_IMPL (shl, <<)

#define GUM_DEFINE_INT64_UNARY_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_int64_##name) \
    { \
      GumQuickInt64 * self; \
      gint64 result; \
      \
      if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      \
      result = op self->value; \
      \
      return _gum_quick_int64_new (ctx, result, core); \
    }

GUM_DEFINE_INT64_UNARY_OP_IMPL (not, ~)

GUMJS_DEFINE_FUNCTION (gumjs_int64_compare)
{
  GumQuickInt64 * self;
  gint64 lhs, rhs;
  gint result;

  if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  lhs = self->value;

  if (!_gum_quick_args_parse (args, "q~", &rhs))
    return JS_EXCEPTION;

  result = (lhs == rhs) ? 0 : ((lhs < rhs) ? -1 : 1);

  return JS_NewInt32 (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_int64_to_number)
{
  GumQuickInt64 * self;

  if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  return JS_NewInt64 (ctx, self->value);
}

GUMJS_DEFINE_FUNCTION (gumjs_int64_to_string)
{
  GumQuickInt64 * self;
  gint64 value;
  gint radix;
  gchar str[32];

  if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  value = self->value;

  radix = 10;
  if (!_gum_quick_args_parse (args, "|u", &radix))
    return JS_EXCEPTION;
  if (radix != 10 && radix != 16)
    return _gum_quick_throw_literal (ctx, "unsupported radix");

  if (radix == 10)
    sprintf (str, "%" G_GINT64_FORMAT, value);
  else if (value >= 0)
    sprintf (str, "%" G_GINT64_MODIFIER "x", value);
  else
    sprintf (str, "-%" G_GINT64_MODIFIER "x", -value);

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_int64_to_json)
{
  GumQuickInt64 * self;
  gchar str[32];

  if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  sprintf (str, "%" G_GINT64_FORMAT, self->value);

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_int64_value_of)
{
  GumQuickInt64 * self;

  if (!_gum_quick_int64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  return JS_NewInt64 (ctx, self->value);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_uint64_construct)
{
  JSValue wrapper;
  guint64 value;
  JSValue proto;
  GumQuickUInt64 * u64;

  if (!_gum_quick_args_parse (args, "Q~", &value))
    return JS_EXCEPTION;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->uint64_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    return JS_EXCEPTION;

  u64 = g_slice_new (GumQuickUInt64);
  u64->value = value;

  JS_SetOpaque (wrapper, u64);

  return wrapper;
}

GUMJS_DEFINE_FINALIZER (gumjs_uint64_finalize)
{
  GumQuickUInt64 * u;

  u = JS_GetOpaque (val, core->uint64_class);
  if (u == NULL)
    return;

  g_slice_free (GumQuickUInt64, u);
}

#define GUM_DEFINE_UINT64_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_uint64_##name) \
    { \
      GumQuickUInt64 * self; \
      guint64 lhs, rhs, result; \
      \
      if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      lhs = self->value; \
      \
      if (!_gum_quick_args_parse (args, "Q~", &rhs)) \
        return JS_EXCEPTION; \
      \
      result = lhs op rhs; \
      \
      return _gum_quick_uint64_new (ctx, result, core); \
    }

GUM_DEFINE_UINT64_OP_IMPL (add, +)
GUM_DEFINE_UINT64_OP_IMPL (sub, -)
GUM_DEFINE_UINT64_OP_IMPL (and, &)
GUM_DEFINE_UINT64_OP_IMPL (or,  |)
GUM_DEFINE_UINT64_OP_IMPL (xor, ^)
GUM_DEFINE_UINT64_OP_IMPL (shr, >>)
GUM_DEFINE_UINT64_OP_IMPL (shl, <<)

#define GUM_DEFINE_UINT64_UNARY_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_uint64_##name) \
    { \
      GumQuickUInt64 * self; \
      guint64 result; \
      \
      if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      \
      result = op self->value; \
      \
      return _gum_quick_uint64_new (ctx, result, core); \
    }

GUM_DEFINE_UINT64_UNARY_OP_IMPL (not, ~)

GUMJS_DEFINE_FUNCTION (gumjs_uint64_compare)
{
  GumQuickUInt64 * self;
  guint64 lhs, rhs;
  gint result;

  if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  lhs = self->value;

  if (!_gum_quick_args_parse (args, "Q~", &rhs))
    return JS_EXCEPTION;

  result = (lhs == rhs) ? 0 : ((lhs < rhs) ? -1 : 1);

  return JS_NewInt32 (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_uint64_to_number)
{
  GumQuickUInt64 * self;

  if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  return JS_NewInt64 (ctx, self->value);
}

GUMJS_DEFINE_FUNCTION (gumjs_uint64_to_string)
{
  GumQuickUInt64 * self;
  guint64 value;
  gint radix;
  gchar str[32];

  if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  value = self->value;

  radix = 10;
  if (!_gum_quick_args_parse (args, "|u", &radix))
    return JS_EXCEPTION;
  if (radix != 10 && radix != 16)
    return _gum_quick_throw_literal (ctx, "unsupported radix");

  if (radix == 10)
    sprintf (str, "%" G_GUINT64_FORMAT, value);
  else
    sprintf (str, "%" G_GINT64_MODIFIER "x", value);

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_uint64_to_json)
{
  GumQuickUInt64 * self;
  gchar str[32];

  if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  sprintf (str, "%" G_GUINT64_FORMAT, self->value);

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_uint64_value_of)
{
  GumQuickUInt64 * self;

  if (!_gum_quick_uint64_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  return JS_NewInt64 (ctx, self->value);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_pointer_construct)
{
  JSValue wrapper;
  gpointer ptr;
  JSValue proto;
  GumQuickNativePointer * np;

  if (!_gum_quick_args_parse (args, "p~", &ptr))
    return JS_EXCEPTION;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->native_pointer_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    return JS_EXCEPTION;

  np = g_slice_new0 (GumQuickNativePointer);
  np->value = ptr;

  JS_SetOpaque (wrapper, np);

  return wrapper;
}

GUMJS_DEFINE_FINALIZER (gumjs_native_pointer_finalize)
{
  GumQuickNativePointer * p;

  p = JS_GetOpaque (val, core->native_pointer_class);
  if (p == NULL)
    return;

  g_slice_free (GumQuickNativePointer, p);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_is_null)
{
  GumQuickNativePointer * self;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  return JS_NewBool (ctx, self->value == NULL);
}

#define GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_##name) \
    { \
      GumQuickNativePointer * self; \
      gpointer lhs_ptr, rhs_ptr; \
      gsize lhs_bits, rhs_bits; \
      gpointer result; \
      \
      if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      lhs_ptr = self->value; \
      \
      if (!_gum_quick_args_parse (args, "p~", &rhs_ptr)) \
        return JS_EXCEPTION; \
      \
      lhs_bits = GPOINTER_TO_SIZE (lhs_ptr); \
      rhs_bits = GPOINTER_TO_SIZE (rhs_ptr); \
      \
      result = GSIZE_TO_POINTER (lhs_bits op rhs_bits); \
      \
      return _gum_quick_native_pointer_new (ctx, result, core); \
    }

GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (add, +)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (sub, -)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (and, &)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (or,  |)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (xor, ^)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (shr, >>)
GUM_DEFINE_NATIVE_POINTER_BINARY_OP_IMPL (shl, <<)

#define GUM_DEFINE_NATIVE_POINTER_UNARY_OP_IMPL(name, op) \
    GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_##name) \
    { \
      GumQuickNativePointer * self; \
      gpointer result; \
      \
      if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self)) \
        return JS_EXCEPTION; \
      \
      result = GSIZE_TO_POINTER (op GPOINTER_TO_SIZE (self->value)); \
      \
      return _gum_quick_native_pointer_new (ctx, result, core); \
    }

GUM_DEFINE_NATIVE_POINTER_UNARY_OP_IMPL (not, ~)

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_sign)
{
#ifdef HAVE_PTRAUTH
  GumQuickNativePointer * self;
  gpointer value;
  const gchar * key;
  gpointer data;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  value = self->value;

  key = "ia";
  data = NULL;
  if (!_gum_quick_args_parse (args, "|sp~", &key, &data))
    return JS_EXCEPTION;

  if (strcmp (key, "ia") == 0)
    value = ptrauth_sign_unauthenticated (value, ptrauth_key_asia, data);
  else if (strcmp (key, "ib") == 0)
    value = ptrauth_sign_unauthenticated (value, ptrauth_key_asib, data);
  else if (strcmp (key, "da") == 0)
    value = ptrauth_sign_unauthenticated (value, ptrauth_key_asda, data);
  else if (strcmp (key, "db") == 0)
    value = ptrauth_sign_unauthenticated (value, ptrauth_key_asdb, data);
  else
    return _gum_quick_throw_literal (ctx, "invalid key");

  return _gum_quick_native_pointer_new (ctx, value, core);
#else
  return JS_DupValue (ctx, this_val);
#endif
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_strip)
{
#ifdef HAVE_PTRAUTH
  GumQuickNativePointer * self;
  gpointer value;
  const gchar * key;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  value = self->value;

  key = "ia";
  if (!_gum_quick_args_parse (args, "|s", &key))
    return JS_EXCEPTION;

  if (strcmp (key, "ia") == 0)
    value = ptrauth_strip (value, ptrauth_key_asia);
  else if (strcmp (key, "ib") == 0)
    value = ptrauth_strip (value, ptrauth_key_asib);
  else if (strcmp (key, "da") == 0)
    value = ptrauth_strip (value, ptrauth_key_asda);
  else if (strcmp (key, "db") == 0)
    value = ptrauth_strip (value, ptrauth_key_asdb);
  else
    return _gum_quick_throw_literal (ctx, "invalid key");

  return _gum_quick_native_pointer_new (ctx, value, core);
#else
  return JS_DupValue (ctx, this_val);
#endif
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_blend)
{
#ifdef HAVE_PTRAUTH
  GumQuickNativePointer * self;
  gpointer value;
  guint small_integer;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  if (!_gum_quick_args_parse (args, "u", &small_integer))
    return JS_EXCEPTION;

  value = GSIZE_TO_POINTER (ptrauth_blend_discriminator (value, small_integer));

  return _gum_quick_native_pointer_new (ctx, value, core);
#else
  return JS_DupValue (ctx, this_val);
#endif
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_compare)
{
  GumQuickNativePointer * self;
  gpointer lhs_ptr, rhs_ptr;
  gsize lhs_bits, rhs_bits;
  gint result;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;
  lhs_ptr = self->value;

  if (!_gum_quick_args_parse (args, "p~", &rhs_ptr))
    return JS_EXCEPTION;

  lhs_bits = GPOINTER_TO_SIZE (lhs_ptr);
  rhs_bits = GPOINTER_TO_SIZE (rhs_ptr);

  result = (lhs_bits == rhs_bits) ? 0 : ((lhs_bits < rhs_bits) ? -1 : 1);

  return JS_NewInt32 (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_int32)
{
  GumQuickNativePointer * self;
  gint32 result;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  result = (gint32) GPOINTER_TO_SIZE (self->value);

  return JS_NewInt32 (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_uint32)
{
  GumQuickNativePointer * self;
  guint32 result;

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  result = (guint32) GPOINTER_TO_SIZE (self->value);

  return JS_NewUint32 (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_string)
{
  GumQuickNativePointer * self;
  gint radix = 0;
  gboolean radix_specified;
  gsize ptr_bits;
  gchar str[32];

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  if (!_gum_quick_args_parse (args, "|u", &radix))
    return JS_EXCEPTION;

  radix_specified = radix != 0;
  if (!radix_specified)
    radix = 16;
  else if (radix != 10 && radix != 16)
    return _gum_quick_throw_literal (ctx, "unsupported radix");

  ptr_bits = GPOINTER_TO_SIZE (self->value);

  if (radix == 10)
  {
    sprintf (str, "%" G_GSIZE_MODIFIER "u", ptr_bits);
  }
  else
  {
    if (radix_specified)
      sprintf (str, "%" G_GSIZE_MODIFIER "x", ptr_bits);
    else
      sprintf (str, "0x%" G_GSIZE_MODIFIER "x", ptr_bits);
  }

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_json)
{
  GumQuickNativePointer * self;
  gchar str[32];

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  sprintf (str, "0x%" G_GSIZE_MODIFIER "x", GPOINTER_TO_SIZE (self->value));

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_match_pattern)
{
  GumQuickNativePointer * self;
  gsize ptr_bits;
  gchar str[24];
  gint src, dst;
  const gint num_bits = GLIB_SIZEOF_VOID_P * 8;
  const gchar nibble_to_char[] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'a', 'b', 'c', 'd', 'e', 'f'
  };

  if (!_gum_quick_native_pointer_unwrap (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  ptr_bits = GPOINTER_TO_SIZE (self->value);

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  for (src = 0, dst = 0; src != num_bits; src += 8)
#else
  for (src = num_bits - 8, dst = 0; src >= 0; src -= 8)
#endif
  {
    if (dst != 0)
      str[dst++] = ' ';
    str[dst++] = nibble_to_char[(ptr_bits >> (src + 4)) & 0xf];
    str[dst++] = nibble_to_char[(ptr_bits >> (src + 0)) & 0xf];
  }
  str[dst] = '\0';

  return JS_NewString (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_array_buffer_wrap)
{
  gpointer address;
  gsize size;

  if (!_gum_quick_args_parse (args, "pZ", &address, &size))
    return JS_EXCEPTION;

  return JS_NewArrayBuffer (ctx, address, size, NULL, NULL, FALSE);
}

GUMJS_DEFINE_FUNCTION (gumjs_array_buffer_unwrap)
{
  uint8_t * address;
  size_t size;

  address = JS_GetArrayBuffer (ctx, &size, this_val);
  if (address == NULL)
    return JS_EXCEPTION;

  return _gum_quick_native_pointer_new (ctx, address, core);
}

GUMJS_DEFINE_FINALIZER (gumjs_native_resource_finalize)
{
  GumQuickNativeResource * r;

  r = JS_GetOpaque (val, core->native_resource_class);
  if (r == NULL)
    return;

  if (r->notify != NULL)
    r->notify (r->native_pointer.value);

  g_slice_free (GumQuickNativeResource, r);
}

GUMJS_DEFINE_FINALIZER (gumjs_kernel_resource_finalize)
{
  GumQuickKernelResource * r;

  r = JS_GetOpaque (val, core->kernel_resource_class);
  if (r == NULL)
    return;

  if (r->notify != NULL)
    r->notify (r->u64.value);

  g_slice_free (GumQuickKernelResource, r);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_function_construct)
{
  JSValue wrapper = JS_NULL;
  GumQuickFFIFunctionParams p = GUM_QUICK_FFI_FUNCTION_PARAMS_EMPTY;
  JSValue proto;
  GumQuickFFIFunction * func;

  if (!gum_quick_ffi_function_params_init (&p, GUM_QUICK_RETURN_PLAIN, args))
    goto propagate_exception;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->native_function_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    goto propagate_exception;

  func = gumjs_ffi_function_new (ctx, &p, core);
  if (func == NULL)
    goto propagate_exception;

  JS_SetOpaque (wrapper, func);

  gum_quick_ffi_function_params_destroy (&p);

  return wrapper;

propagate_exception:
  {
    JS_FreeValue (ctx, wrapper);
    gum_quick_ffi_function_params_destroy (&p);

    return JS_EXCEPTION;
  }
}

GUMJS_DEFINE_FINALIZER (gumjs_native_function_finalize)
{
  GumQuickFFIFunction * f;

  f = JS_GetOpaque (val, core->native_function_class);
  if (f == NULL)
    return;

  gum_quick_ffi_function_finalize (f);
}

GUMJS_DEFINE_CALL_HANDLER (gumjs_native_function_invoke)
{
  return gumjs_ffi_function_invoke (ctx, func_obj, core->native_function_class,
      args, core);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_function_call)
{
  return gumjs_ffi_function_call (ctx, this_val, core->native_function_class,
      args, core);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_function_apply)
{
  return gumjs_ffi_function_apply (ctx, this_val, core->native_function_class,
      args, core);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_system_function_construct)
{
  JSValue wrapper = JS_NULL;
  GumQuickFFIFunctionParams p = GUM_QUICK_FFI_FUNCTION_PARAMS_EMPTY;
  JSValue proto;
  GumQuickFFIFunction * func;

  if (!gum_quick_ffi_function_params_init (&p, GUM_QUICK_RETURN_DETAILED, args))
    goto propagate_exception;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->system_function_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    goto propagate_exception;

  func = gumjs_ffi_function_new (ctx, &p, core);
  if (func == NULL)
    goto propagate_exception;

  JS_SetOpaque (wrapper, func);

  gum_quick_ffi_function_params_destroy (&p);

  return wrapper;

propagate_exception:
  {
    JS_FreeValue (ctx, wrapper);
    gum_quick_ffi_function_params_destroy (&p);

    return JS_EXCEPTION;
  }
}

GUMJS_DEFINE_FINALIZER (gumjs_system_function_finalize)
{
  GumQuickFFIFunction * f;

  f = JS_GetOpaque (val, core->system_function_class);
  if (f == NULL)
    return;

  gum_quick_ffi_function_finalize (f);
}

GUMJS_DEFINE_CALL_HANDLER (gumjs_system_function_invoke)
{
  return gumjs_ffi_function_invoke (ctx, func_obj, core->system_function_class,
      args, core);
}

GUMJS_DEFINE_FUNCTION (gumjs_system_function_call)
{
  return gumjs_ffi_function_call (ctx, this_val, core->system_function_class,
      args, core);
}

GUMJS_DEFINE_FUNCTION (gumjs_system_function_apply)
{
  return gumjs_ffi_function_apply (ctx, this_val, core->system_function_class,
      args, core);
}

static GumQuickFFIFunction *
gumjs_ffi_function_new (JSContext * ctx,
                        const GumQuickFFIFunctionParams * params,
                        GumQuickCore * core)
{
  GumQuickFFIFunction * func;
  GumQuickNativePointer * ptr;
  ffi_type * rtype;
  JSValue val = JS_UNDEFINED;
  guint nargs_fixed, nargs_total, length, i;
  gboolean is_variadic;
  ffi_abi abi;

  func = g_slice_new0 (GumQuickFFIFunction);
  ptr = &func->native_pointer;
  ptr->value = GUM_FUNCPTR_TO_POINTER (params->implementation);
  func->implementation = params->implementation;
  func->scheduling = params->scheduling;
  func->exceptions = params->exceptions;
  func->traps = params->traps;
  func->return_shape = params->return_shape;

  if (!gum_quick_ffi_type_get (ctx, params->return_type, core, &rtype,
      &func->data))
    goto invalid_return_type;

  if (!_gum_quick_array_get_length (ctx, params->argument_types, core, &length))
    goto invalid_argument_array;

  nargs_fixed = nargs_total = length;
  is_variadic = FALSE;

  func->atypes = g_new (ffi_type *, nargs_total);

  for (i = 0; i != nargs_total; i++)
  {
    gboolean is_marker;

    val = JS_GetPropertyUint32 (ctx, params->argument_types, i);
    if (JS_IsException (val))
      goto invalid_argument_array;

    if (JS_IsString (val))
    {
      const char * str = JS_ToCString (ctx, val);
      is_marker = strcmp (str, "...") == 0;
      JS_FreeCString (ctx, str);
    }
    else
    {
      is_marker = FALSE;
    }

    if (is_marker)
    {
      if (i == 0 || is_variadic)
        goto unexpected_marker;

      nargs_fixed = i;
      is_variadic = TRUE;
    }
    else
    {
      ffi_type ** atype;

      atype = &func->atypes[is_variadic ? i - 1 : i];

      if (!gum_quick_ffi_type_get (ctx, val, core, atype, &func->data))
        goto invalid_argument_type;

      if (is_variadic)
        *atype = gum_ffi_maybe_promote_variadic (*atype);
    }

    JS_FreeValue (ctx, val);
    val = JS_UNDEFINED;
  }

  if (is_variadic)
    nargs_total--;

  if (params->abi_name != NULL)
  {
    if (!gum_quick_ffi_abi_get (ctx, params->abi_name, &abi))
      goto invalid_abi;
  }
  else
  {
    abi = FFI_DEFAULT_ABI;
  }

  if (is_variadic)
  {
    if (ffi_prep_cif_var (&func->cif, abi, (guint) nargs_fixed,
        (guint) nargs_total, rtype, func->atypes) != FFI_OK)
      goto compilation_failed;
  }
  else
  {
    if (ffi_prep_cif (&func->cif, abi, (guint) nargs_total, rtype,
        func->atypes) != FFI_OK)
      goto compilation_failed;
  }

  func->is_variadic = nargs_fixed < nargs_total;
  func->nargs_fixed = nargs_fixed;
  func->abi = abi;

  for (i = 0; i != nargs_total; i++)
  {
    ffi_type * t = func->atypes[i];

    func->arglist_size = GUM_ALIGN_SIZE (func->arglist_size, t->alignment);
    func->arglist_size += t->size;
  }

  return func;

invalid_return_type:
invalid_argument_array:
invalid_argument_type:
invalid_abi:
  {
    JS_FreeValue (ctx, val);
    gum_quick_ffi_function_finalize (func);

    return NULL;
  }
unexpected_marker:
  {
    JS_FreeValue (ctx, val);
    gum_quick_ffi_function_finalize (func);

    _gum_quick_throw_literal (ctx, "only one variadic marker may be specified, "
        "and can not be the first argument");
    return NULL;
  }
compilation_failed:
  {
    gum_quick_ffi_function_finalize (func);

    _gum_quick_throw_literal (ctx, "failed to compile function call interface");
    return NULL;
  }
}

static void
gum_quick_ffi_function_finalize (GumQuickFFIFunction * func)
{
  while (func->data != NULL)
  {
    GSList * head = func->data;
    g_free (head->data);
    func->data = g_slist_delete_link (func->data, head);
  }
  g_free (func->atypes);

  g_slice_free (GumQuickFFIFunction, func);
}

static JSValue
gum_quick_ffi_function_invoke (GumQuickFFIFunction * self,
                               JSContext * ctx,
                               GCallback implementation,
                               guint argc,
                               JSValueConst * argv,
                               GumQuickCore * core)
{
  JSValue result;
  ffi_cif * cif;
  guint nargs, nargs_fixed;
  gboolean is_variadic;
  ffi_type * rtype;
  ffi_type ** atypes;
  gsize rsize, ralign;
  GumFFIValue * rvalue;
  void ** avalue;
  guint8 * avalues;
  ffi_cif tmp_cif;
  GumFFIValue tmp_value = { 0, };
  GumQuickSchedulingBehavior scheduling;
  GumQuickExceptionsBehavior exceptions;
  GumQuickCodeTraps traps;
  GumQuickReturnValueShape return_shape;
  GumExceptorScope exceptor_scope;
  GumInvocationState invocation_state;
  gint system_error;

  cif = &self->cif;
  nargs = cif->nargs;
  nargs_fixed = self->nargs_fixed;
  is_variadic = self->is_variadic;

  if ((is_variadic && argc < nargs_fixed) || (!is_variadic && argc != nargs))
    return _gum_quick_throw_literal (ctx, "bad argument count");

  rtype = cif->rtype;
  atypes = cif->arg_types;
  rsize = MAX (rtype->size, sizeof (gsize));
  ralign = MAX (rtype->alignment, sizeof (gsize));
  rvalue = g_alloca (rsize + ralign - 1);
  rvalue = GUM_ALIGN_POINTER (GumFFIValue *, rvalue, ralign);

  if (argc > 0)
  {
    gsize arglist_size, arglist_alignment, offset, i;

    avalue = g_newa (void *, MAX (nargs, argc));

    arglist_size = self->arglist_size;
    if (is_variadic && argc > nargs)
    {
      gsize type_idx;

      atypes = g_newa (ffi_type *, argc);

      memcpy (atypes, cif->arg_types, nargs * sizeof (void *));
      for (i = nargs, type_idx = nargs_fixed; i != argc; i++)
      {
        ffi_type * t = cif->arg_types[type_idx];

        atypes[i] = t;
        arglist_size = GUM_ALIGN_SIZE (arglist_size, t->alignment);
        arglist_size += t->size;

        if (++type_idx >= nargs)
          type_idx = nargs_fixed;
      }

      cif = &tmp_cif;
      if (ffi_prep_cif_var (cif, self->abi, (guint) nargs_fixed,
          (guint) argc, rtype, atypes) != FFI_OK)
      {
        return _gum_quick_throw_literal (ctx,
            "failed to compile function call interface");
      }
    }

    arglist_alignment = atypes[0]->alignment;
    avalues = g_alloca (arglist_size + arglist_alignment - 1);
    avalues = GUM_ALIGN_POINTER (guint8 *, avalues, arglist_alignment);

    /* Prefill with zero to clear high bits of values smaller than a pointer. */
    memset (avalues, 0, arglist_size);

    offset = 0;
    for (i = 0; i != argc; i++)
    {
      ffi_type * t;
      GumFFIValue * v;

      t = atypes[i];
      offset = GUM_ALIGN_SIZE (offset, t->alignment);
      v = (GumFFIValue *) (avalues + offset);

      if (!gum_quick_value_to_ffi (ctx, argv[i], t, core, v))
        return JS_EXCEPTION;
      avalue[i] = v;

      offset += t->size;
    }

    while (i < nargs)
      avalue[i++] = &tmp_value;
  }
  else
  {
    avalue = NULL;
  }

  scheduling = self->scheduling;
  exceptions = self->exceptions;
  traps = self->traps;
  return_shape = self->return_shape;
  system_error = -1;

  {
    GumQuickScope scope = GUM_QUICK_SCOPE_INIT (core);
    GumInterceptor * interceptor = core->interceptor->interceptor;
    GumStalker * stalker = NULL;

    if (exceptions == GUM_QUICK_EXCEPTIONS_PROPAGATE ||
        gum_exceptor_try (core->exceptor, &exceptor_scope))
    {
      if (exceptions == GUM_QUICK_EXCEPTIONS_STEAL)
        gum_interceptor_save (&invocation_state);

      if (scheduling == GUM_QUICK_SCHEDULING_COOPERATIVE)
      {
        _gum_quick_scope_suspend (&scope);

        gum_interceptor_unignore_current_thread (interceptor);
      }

      if (traps == GUM_QUICK_CODE_TRAPS_ALL)
      {
        _gum_quick_stalker_process_pending (core->stalker,
            scope.previous_scope);

        stalker = _gum_quick_stalker_get (core->stalker);
        gum_stalker_activate (stalker,
            GUM_FUNCPTR_TO_POINTER (implementation));
      }

      ffi_call (cif, implementation, rvalue, avalue);

      g_clear_pointer (&stalker, gum_stalker_deactivate);

      if (return_shape == GUM_QUICK_RETURN_DETAILED)
        system_error = gum_thread_get_system_error ();
    }

    g_clear_pointer (&stalker, gum_stalker_deactivate);

    if (scheduling == GUM_QUICK_SCHEDULING_COOPERATIVE)
    {
      gum_interceptor_ignore_current_thread (interceptor);

      _gum_quick_scope_resume (&scope);
    }
  }

  if (exceptions == GUM_QUICK_EXCEPTIONS_STEAL &&
      gum_exceptor_catch (core->exceptor, &exceptor_scope))
  {
    gum_interceptor_restore (&invocation_state);

    return _gum_quick_throw_native (ctx, &exceptor_scope.exception, core);
  }

  result = gum_quick_value_from_ffi (ctx, rvalue, rtype, core);

  if (return_shape == GUM_QUICK_RETURN_DETAILED)
  {
    JSValue d = JS_NewObject (ctx);
    JS_DefinePropertyValue (ctx, d,
        GUM_QUICK_CORE_ATOM (core, value),
        result,
        JS_PROP_C_W_E);
    JS_DefinePropertyValue (ctx, d,
        GUM_QUICK_CORE_ATOM (core, system_error),
        JS_NewInt32 (ctx, system_error),
        JS_PROP_C_W_E);
    return d;
  }
  else
  {
    return result;
  }
}

static JSValue
gumjs_ffi_function_invoke (JSContext * ctx,
                           JSValueConst func_obj,
                           JSClassID klass,
                           GumQuickArgs * args,
                           GumQuickCore * core)
{
  GumQuickFFIFunction * self;

  if (!_gum_quick_unwrap (ctx, func_obj, klass, core, (gpointer *) &self))
    return JS_EXCEPTION;

  return gum_quick_ffi_function_invoke (self, ctx, self->implementation,
      args->count, args->elements, core);
}

static JSValue
gumjs_ffi_function_call (JSContext * ctx,
                         JSValueConst func_obj,
                         JSClassID klass,
                         GumQuickArgs * args,
                         GumQuickCore * core)
{
  const int argc = args->count;
  JSValueConst * argv = args->elements;
  JSValue receiver;
  GumQuickFFIFunction * func;
  GCallback impl;

  if (argc == 0 || JS_IsNull (argv[0]) || JS_IsUndefined (argv[0]))
  {
    receiver = JS_NULL;
  }
  else if (JS_IsObject (argv[0]))
  {
    receiver = argv[0];
  }
  else
  {
    return _gum_quick_throw_literal (ctx, "invalid receiver");
  }

  if (!gumjs_ffi_function_get (ctx, func_obj, receiver, klass, core, &func,
      &impl))
  {
    return JS_EXCEPTION;
  }

  return gum_quick_ffi_function_invoke (func, ctx, impl, MAX (argc - 1, 0),
      argv + 1, core);
}

static JSValue
gumjs_ffi_function_apply (JSContext * ctx,
                          JSValueConst func_obj,
                          JSClassID klass,
                          GumQuickArgs * args,
                          GumQuickCore * core)
{
  JSValueConst * argv = args->elements;
  JSValue receiver;
  GumQuickFFIFunction * func;
  GCallback impl;
  guint n, i;
  JSValue * values;

  if (JS_IsNull (argv[0]) || JS_IsUndefined (argv[0]))
  {
    receiver = JS_NULL;
  }
  else if (JS_IsObject (argv[0]))
  {
    receiver = argv[0];
  }
  else
  {
    return _gum_quick_throw_literal (ctx, "invalid receiver");
  }

  if (!gumjs_ffi_function_get (ctx, func_obj, receiver, klass, core, &func,
      &impl))
  {
    return JS_EXCEPTION;
  }

  if (JS_IsNull (argv[1]) || JS_IsUndefined (argv[1]))
  {
    return gum_quick_ffi_function_invoke (func, ctx, impl, 0, NULL, core);
  }
  else
  {
    JSValueConst elements = argv[1];
    JSValue result;

    if (!_gum_quick_array_get_length (ctx, elements, core, &n))
      return JS_EXCEPTION;

    values = g_newa (JSValue, n);

    for (i = 0; i != n; i++)
    {
      values[i] = JS_GetPropertyUint32 (ctx, elements, i);
      if (JS_IsException (values[i]))
        goto invalid_argument_value;
    }

    result = gum_quick_ffi_function_invoke (func, ctx, impl, n, values, core);

    for (i = 0; i != n; i++)
      JS_FreeValue (ctx, values[i]);

    return result;
  }

invalid_argument_value:
  {
    n = i;
    for (i = 0; i != n; i++)
      JS_FreeValue (ctx, values[i]);

    return JS_EXCEPTION;
  }
}

static gboolean
gumjs_ffi_function_get (JSContext * ctx,
                        JSValueConst func_obj,
                        JSValueConst receiver,
                        JSClassID klass,
                        GumQuickCore * core,
                        GumQuickFFIFunction ** func,
                        GCallback * implementation)
{
  GumQuickFFIFunction * f;

  if (_gum_quick_try_unwrap (func_obj, klass, core, (gpointer *) &f))
  {
    *func = f;

    if (!JS_IsNull (receiver))
    {
      gpointer impl;
      if (!_gum_quick_native_pointer_get (ctx, receiver, core, &impl))
        return FALSE;
      *implementation = GUM_POINTER_TO_FUNCPTR (GCallback, impl);
    }
    else
    {
      *implementation = f->implementation;
    }
  }
  else
  {
    if (!_gum_quick_unwrap (ctx, receiver, klass, core, (gpointer *) &f))
      return FALSE;

    *func = f;
    *implementation = f->implementation;
  }

  return TRUE;
}

static gboolean
gum_quick_ffi_function_params_init (GumQuickFFIFunctionParams * params,
                                    GumQuickReturnValueShape return_shape,
                                    GumQuickArgs * args)
{
  JSContext * ctx = args->ctx;
  JSValueConst abi_or_options;
  JSValue val;

  params->ctx = ctx;

  abi_or_options = JS_UNDEFINED;
  if (!_gum_quick_args_parse (args, "pVA|V", &params->implementation,
      &params->return_type, &params->argument_types, &abi_or_options))
  {
    return FALSE;
  }
  params->abi_name = NULL;
  params->scheduling = GUM_QUICK_SCHEDULING_COOPERATIVE;
  params->exceptions = GUM_QUICK_EXCEPTIONS_STEAL;
  params->traps = GUM_QUICK_CODE_TRAPS_DEFAULT;
  params->return_shape = return_shape;

  if (JS_IsString (abi_or_options))
  {
    JSValueConst abi = abi_or_options;

    params->abi_name = JS_ToCString (ctx, abi);
  }
  else if (JS_IsObject (abi_or_options))
  {
    JSValueConst options = abi_or_options;
    GumQuickCore * core = args->core;

    val = JS_GetProperty (ctx, options, GUM_QUICK_CORE_ATOM (core, abi));
    if (JS_IsException (val))
      goto invalid_value;
    if (!JS_IsUndefined (val))
    {
      params->abi_name = JS_ToCString (ctx, val);
      if (params->abi_name == NULL)
        goto invalid_value;
      JS_FreeValue (ctx, val);
    }

    val = JS_GetProperty (ctx, options, GUM_QUICK_CORE_ATOM (core, scheduling));
    if (JS_IsException (val))
      goto invalid_value;
    if (!JS_IsUndefined (val))
    {
      if (!gum_quick_scheduling_behavior_get (ctx, val, &params->scheduling))
        goto invalid_value;
      JS_FreeValue (ctx, val);
    }

    val = JS_GetProperty (ctx, options, GUM_QUICK_CORE_ATOM (core, exceptions));
    if (JS_IsException (val))
      goto invalid_value;
    if (!JS_IsUndefined (val))
    {
      if (!gum_quick_exceptions_behavior_get (ctx, val, &params->exceptions))
        goto invalid_value;
      JS_FreeValue (ctx, val);
    }

    val = JS_GetProperty (ctx, options, GUM_QUICK_CORE_ATOM (core, traps));
    if (JS_IsException (val))
      goto invalid_value;
    if (!JS_IsUndefined (val))
    {
      if (!gum_quick_code_traps_get (ctx, val, &params->traps))
        goto invalid_value;
      JS_FreeValue (ctx, val);
    }
  }
  else if (!JS_IsUndefined (abi_or_options))
  {
    _gum_quick_throw_literal (ctx,
        "expected string or object containing options");
    return FALSE;
  }

  return TRUE;

invalid_value:
  {
    JS_FreeValue (ctx, val);
    JS_FreeCString (ctx, params->abi_name);

    return FALSE;
  }
}

static void
gum_quick_ffi_function_params_destroy (GumQuickFFIFunctionParams * params)
{
  JSContext * ctx = params->ctx;

  JS_FreeCString (ctx, params->abi_name);
}

static gboolean
gum_quick_scheduling_behavior_get (JSContext * ctx,
                                   JSValueConst val,
                                   GumQuickSchedulingBehavior * behavior)
{
  const char * str;

  str = JS_ToCString (ctx, val);
  if (str == NULL)
    return FALSE;

  if (strcmp (str, "cooperative") == 0)
    *behavior = GUM_QUICK_SCHEDULING_COOPERATIVE;
  else if (strcmp (str, "exclusive") == 0)
    *behavior = GUM_QUICK_SCHEDULING_EXCLUSIVE;
  else
    goto invalid_value;

  JS_FreeCString (ctx, str);

  return TRUE;

invalid_value:
  {
    JS_FreeCString (ctx, str);

    _gum_quick_throw_literal (ctx, "invalid scheduling behavior value");
    return FALSE;
  }
}

static gboolean
gum_quick_exceptions_behavior_get (JSContext * ctx,
                                   JSValueConst val,
                                   GumQuickExceptionsBehavior * behavior)
{
  const char * str;

  str = JS_ToCString (ctx, val);
  if (str == NULL)
    return FALSE;

  if (strcmp (str, "steal") == 0)
    *behavior = GUM_QUICK_EXCEPTIONS_STEAL;
  else if (strcmp (str, "propagate") == 0)
    *behavior = GUM_QUICK_EXCEPTIONS_PROPAGATE;
  else
    goto invalid_value;

  JS_FreeCString (ctx, str);

  return TRUE;

invalid_value:
  {
    JS_FreeCString (ctx, str);

    _gum_quick_throw_literal (ctx, "invalid exceptions behavior value");
    return FALSE;
  }
}

static gboolean
gum_quick_code_traps_get (JSContext * ctx,
                          JSValueConst val,
                          GumQuickCodeTraps * traps)
{
  const char * str;

  str = JS_ToCString (ctx, val);
  if (str == NULL)
    return FALSE;

  if (strcmp (str, "all") == 0)
    *traps = GUM_QUICK_CODE_TRAPS_ALL;
  else if (strcmp (str, "default") == 0)
    *traps = GUM_QUICK_CODE_TRAPS_DEFAULT;
  else
    goto invalid_value;

  JS_FreeCString (ctx, str);

  return TRUE;

invalid_value:
  {
    JS_FreeCString (ctx, str);

    _gum_quick_throw_literal (ctx, "invalid code traps value");
    return FALSE;
  }
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_callback_construct)
{
  JSValue wrapper = JS_NULL;
  JSValue func, rtype_value, atypes_array, proto;
  gchar * abi_str = NULL;
  GumQuickNativeCallback * cb = NULL;
  GumQuickNativePointer * ptr;
  ffi_type * rtype;
  guint nargs, i;
  JSValue val = JS_NULL;
  ffi_abi abi;

  if (!_gum_quick_args_parse (args, "FVA|s", &func, &rtype_value, &atypes_array,
      &abi_str))
    goto propagate_exception;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->native_callback_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    goto propagate_exception;

  cb = g_slice_new0 (GumQuickNativeCallback);
  ptr = &cb->native_pointer;
  cb->func = func;
  cb->core = core;

  if (!gum_quick_ffi_type_get (ctx, rtype_value, core, &rtype, &cb->data))
    goto propagate_exception;

  if (!_gum_quick_array_get_length (ctx, atypes_array, core, &nargs))
    goto propagate_exception;

  cb->atypes = g_new (ffi_type *, nargs);

  for (i = 0; i != nargs; i++)
  {
    ffi_type ** atype;

    val = JS_GetPropertyUint32 (ctx, atypes_array, i);
    if (JS_IsException (val))
      goto propagate_exception;

    atype = &cb->atypes[i];

    if (!gum_quick_ffi_type_get (ctx, val, core, atype, &cb->data))
      goto propagate_exception;

    JS_FreeValue (ctx, val);
    val = JS_NULL;
  }

  if (abi_str != NULL)
  {
    if (!gum_quick_ffi_abi_get (ctx, abi_str, &abi))
      goto propagate_exception;
  }
  else
  {
    abi = FFI_DEFAULT_ABI;
  }

  cb->closure = ffi_closure_alloc (sizeof (ffi_closure), &ptr->value);
  if (cb->closure == NULL)
    goto alloc_failed;

  if (ffi_prep_cif (&cb->cif, abi, (guint) nargs, rtype, cb->atypes) != FFI_OK)
    goto compilation_failed;

  if (ffi_prep_closure_loc (cb->closure, &cb->cif,
      gum_quick_native_callback_invoke, cb, ptr->value) != FFI_OK)
    goto prepare_failed;

  JS_SetOpaque (wrapper, cb);
  JS_DefinePropertyValueStr (ctx, wrapper, "$f", JS_DupValue (ctx, func), 0);

  return wrapper;

alloc_failed:
  {
    _gum_quick_throw_literal (ctx, "failed to allocate closure");
    goto propagate_exception;
  }
compilation_failed:
  {
    _gum_quick_throw_literal (ctx, "failed to compile function call interface");
    goto propagate_exception;
  }
prepare_failed:
  {
    _gum_quick_throw_literal (ctx, "failed to prepare closure");
    goto propagate_exception;
  }
propagate_exception:
  {
    JS_FreeValue (ctx, val);
    if (cb != NULL)
      gum_quick_native_callback_finalize (cb);
    JS_FreeValue (ctx, wrapper);

    return JS_EXCEPTION;
  }
}

GUMJS_DEFINE_FINALIZER (gumjs_native_callback_finalize)
{
  GumQuickNativeCallback * c;

  c = JS_GetOpaque (val, core->native_callback_class);
  if (c == NULL)
    return;

  gum_quick_native_callback_finalize (c);
}

static void
gum_quick_native_callback_finalize (GumQuickNativeCallback * callback)
{
  ffi_closure_free (callback->closure);

  while (callback->data != NULL)
  {
    GSList * head = callback->data;
    g_free (head->data);
    callback->data = g_slist_delete_link (callback->data, head);
  }
  g_free (callback->atypes);

  g_slice_free (GumQuickNativeCallback, callback);
}

static void
gum_quick_native_callback_invoke (ffi_cif * cif,
                                  void * return_value,
                                  void ** args,
                                  void * user_data)
{
  GumQuickNativeCallback * self = user_data;
  GumQuickCore * core = self->core;
  GumQuickScope scope;
  JSContext * ctx = core->ctx;
  ffi_type * rtype = cif->rtype;
  GumFFIValue * retval = return_value;
  GumInvocationContext * ic;
  GumQuickInvocationContext * jic = NULL;
  JSValue this_obj;
  int argc, i;
  JSValue * argv;
  JSValue result;

  _gum_quick_scope_enter (&scope, core);

  if (rtype != &ffi_type_void)
  {
    /*
     * Ensure:
     * - high bits of values smaller than a pointer are cleared to zero
     * - we return something predictable in case of a JS exception
     */
    retval->v_pointer = NULL;
  }

  ic = gum_interceptor_get_current_invocation ();
  if (ic != NULL)
  {
    jic = _gum_quick_interceptor_obtain_invocation_context (core->interceptor);
    _gum_quick_invocation_context_reset (jic, ic);

    this_obj = jic->wrapper;
  }
  else
  {
    this_obj = JS_UNDEFINED;
  }

  argc = cif->nargs;
  argv = g_newa (JSValue, argc);

  for (i = 0; i != argc; i++)
    argv[i] = gum_quick_value_from_ffi (ctx, args[i], cif->arg_types[i], core);

  result = _gum_quick_scope_call (&scope, self->func, this_obj, argc, argv);

  for (i = 0; i != argc; i++)
    JS_FreeValue (ctx, argv[i]);

  if (jic != NULL)
  {
    _gum_quick_invocation_context_reset (jic, NULL);
    _gum_quick_interceptor_release_invocation_context (core->interceptor, jic);
  }

  if (!JS_IsException (result) && cif->rtype != &ffi_type_void)
  {
    if (!gum_quick_value_to_ffi (ctx, result, cif->rtype, core, retval))
      _gum_quick_scope_catch_and_emit (&scope);
  }

  JS_FreeValue (ctx, result);

  _gum_quick_scope_leave (&scope);
}

GUMJS_DEFINE_FINALIZER (gumjs_cpu_context_finalize)
{
  GumQuickCpuContext * c;

  c = JS_GetOpaque (val, core->cpu_context_class);
  if (c == NULL)
    return;

  g_slice_free (GumQuickCpuContext, c);
}

GUMJS_DEFINE_FUNCTION (gumjs_cpu_context_to_json)
{
  JSValue result;
  guint i;

  result = JS_NewObject (ctx);

  for (i = 0; i != G_N_ELEMENTS (gumjs_cpu_context_entries); i++)
  {
    const JSCFunctionListEntry * e = &gumjs_cpu_context_entries[i];
    JSValue val;

    if (e->def_type != JS_DEF_CGETSET)
      continue;

    val = JS_GetPropertyStr (ctx, this_val, e->name);
    if (JS_IsException (val))
      goto propagate_exception;
    JS_SetPropertyStr (ctx, result, e->name, val);
  }

  return result;

propagate_exception:
  {
    JS_FreeValue (ctx, result);

    return JS_EXCEPTION;
  }
}

static JSValue
gumjs_cpu_context_set_register (GumQuickCpuContext * self,
                                JSContext * ctx,
                                JSValueConst val,
                                gpointer * reg)
{
  if (self->access == GUM_CPU_CONTEXT_READONLY)
    return _gum_quick_throw_literal (ctx, "invalid operation");

  return _gum_quick_native_pointer_parse (ctx, val, self->core, reg)
      ? JS_UNDEFINED
      : JS_EXCEPTION;
}

static gboolean
gum_quick_source_map_get (JSContext * ctx,
                          JSValueConst val,
                          GumQuickCore * core,
                          GumSourceMap ** source_map)
{
  return _gum_quick_unwrap (ctx, val, core->source_map_class, core,
      (gpointer *) source_map);
}

static JSValue
gumjs_source_map_new (const gchar * json,
                      GumQuickCore * core)
{
  JSValue result;
  JSContext * ctx = core->ctx;
  JSValue json_val;

  json_val = JS_NewString (ctx, json);

  result = JS_CallConstructor (ctx, core->source_map_ctor, 1, &json_val);

  JS_FreeValue (ctx, json_val);

  return result;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_source_map_construct)
{
  JSValue wrapper = JS_NULL;
  const gchar * json;
  JSValue proto;
  GumSourceMap * map;

  if (!_gum_quick_args_parse (args, "s", &json))
    goto propagate_exception;

  proto = JS_GetProperty (ctx, new_target,
      GUM_QUICK_CORE_ATOM (core, prototype));
  wrapper = JS_NewObjectProtoClass (ctx, proto, core->source_map_class);
  JS_FreeValue (ctx, proto);
  if (JS_IsException (wrapper))
    goto propagate_exception;

  map = gum_source_map_new (json);
  if (map == NULL)
    goto invalid_source_map;

  JS_SetOpaque (wrapper, map);

  return wrapper;

invalid_source_map:
  {
    _gum_quick_throw_literal (ctx, "invalid source map");
    goto propagate_exception;
  }
propagate_exception:
  {
    JS_FreeValue (ctx, wrapper);

    return JS_EXCEPTION;
  }
}

GUMJS_DEFINE_FINALIZER (gumjs_source_map_finalize)
{
  GumSourceMap * m;

  m = JS_GetOpaque (val, core->source_map_class);
  if (m == NULL)
    return;

  g_object_unref (m);
}

GUMJS_DEFINE_FUNCTION (gumjs_source_map_resolve)
{
  GumSourceMap * self;
  guint line, column;
  const gchar * source, * name;

  if (!gum_quick_source_map_get (ctx, this_val, core, &self))
    return JS_EXCEPTION;

  if (args->count == 1)
  {
    if (!_gum_quick_args_parse (args, "u", &line))
      return JS_EXCEPTION;
    column = G_MAXUINT;
  }
  else
  {
    if (!_gum_quick_args_parse (args, "uu", &line, &column))
      return JS_EXCEPTION;
  }

  if (gum_source_map_resolve (self, &line, &column, &source, &name))
  {
    JSValue pos;
    const int fl = JS_PROP_C_W_E;

    pos = JS_NewArray (ctx);
    JS_DefinePropertyValueUint32 (ctx, pos, 0, JS_NewString (ctx, source), fl);
    JS_DefinePropertyValueUint32 (ctx, pos, 1, JS_NewUint32 (ctx, line), fl);
    JS_DefinePropertyValueUint32 (ctx, pos, 2, JS_NewUint32 (ctx, column), fl);
    JS_DefinePropertyValueUint32 (ctx, pos, 3,
        (name != NULL) ? JS_NewString (ctx, name) : JS_NULL, fl);

    return pos;
  }
  else
  {
    return JS_NULL;
  }
}

static GumQuickWeakRef *
gum_quick_weak_ref_new (guint id,
                        JSValue callback,
                        GumQuickCore * core)
{
  GumQuickWeakRef * ref;

  ref = g_slice_new (GumQuickWeakRef);
  ref->id = id;
  ref->callback = callback;
  ref->core = core;

  return ref;
}

static void
gum_quick_weak_ref_clear (GumQuickWeakRef * ref)
{
  GumQuickCore * core = ref->core;

  _gum_quick_scope_call_void (core->current_scope, ref->callback, JS_UNDEFINED,
      0, NULL);

  ref->callback = JS_NULL;
  ref->core = NULL;
}

static JSValue
gum_quick_core_schedule_callback (GumQuickCore * self,
                                  GumQuickArgs * args,
                                  gboolean repeat)
{
  JSValue func;
  gsize delay;
  guint id;
  GSource * source;
  GumQuickScheduledCallback * callback;

  if (repeat)
  {
    if (!_gum_quick_args_parse (args, "FZ", &func, &delay))
      return JS_EXCEPTION;
  }
  else
  {
    delay = 0;
    if (!_gum_quick_args_parse (args, "F|Z", &func, &delay))
      return JS_EXCEPTION;
  }

  id = self->next_callback_id++;
  if (delay == 0)
    source = g_idle_source_new ();
  else
    source = g_timeout_source_new ((guint) delay);

  callback = gum_scheduled_callback_new (id, func, repeat, source, self);
  g_source_set_callback (source, (GSourceFunc) gum_scheduled_callback_invoke,
      callback, (GDestroyNotify) gum_scheduled_callback_free);

  g_hash_table_insert (self->scheduled_callbacks, GINT_TO_POINTER (id),
      callback);
  g_queue_push_tail (&self->current_scope->scheduled_sources, source);

  return JS_NewUint32 (self->ctx, id);
}

static GumQuickScheduledCallback *
gum_quick_core_try_steal_scheduled_callback (GumQuickCore * self,
                                             gint id)
{
  GumQuickScheduledCallback * callback;
  gpointer raw_id;

  raw_id = GINT_TO_POINTER (id);

  callback = g_hash_table_lookup (self->scheduled_callbacks, raw_id);
  if (callback == NULL)
    return NULL;

  g_hash_table_remove (self->scheduled_callbacks, raw_id);

  return callback;
}

static GumQuickScheduledCallback *
gum_scheduled_callback_new (guint id,
                            JSValueConst func,
                            gboolean repeat,
                            GSource * source,
                            GumQuickCore * core)
{
  GumQuickScheduledCallback * cb;

  cb = g_slice_new (GumQuickScheduledCallback);
  cb->id = id;
  cb->func = JS_DupValue (core->ctx, func);
  cb->repeat = repeat;
  cb->source = source;
  cb->core = core;

  return cb;
}

static void
gum_scheduled_callback_free (GumQuickScheduledCallback * callback)
{
  GumQuickCore * core = callback->core;
  GumQuickScope scope;

  _gum_quick_scope_enter (&scope, core);
  _gum_quick_core_unpin (core);
  JS_FreeValue (core->ctx, callback->func);
  _gum_quick_scope_leave (&scope);

  g_slice_free (GumQuickScheduledCallback, callback);
}

static gboolean
gum_scheduled_callback_invoke (GumQuickScheduledCallback * self)
{
  GumQuickCore * core = self->core;
  GumQuickScope scope;

  _gum_quick_scope_enter (&scope, self->core);

  _gum_quick_scope_call_void (&scope, self->func, JS_UNDEFINED, 0, NULL);

  if (!self->repeat)
  {
    if (gum_quick_core_try_steal_scheduled_callback (core, self->id) != NULL)
      _gum_quick_core_pin (core);
  }

  _gum_quick_scope_leave (&scope);

  return self->repeat;
}

static GumQuickExceptionSink *
gum_quick_exception_sink_new (JSValueConst callback,
                              GumQuickCore * core)
{
  GumQuickExceptionSink * sink;

  sink = g_slice_new (GumQuickExceptionSink);
  sink->callback = JS_DupValue (core->ctx, callback);
  sink->core = core;

  return sink;
}

static void
gum_quick_exception_sink_free (GumQuickExceptionSink * sink)
{
  JS_FreeValue (sink->core->ctx, sink->callback);

  g_slice_free (GumQuickExceptionSink, sink);
}

static void
gum_quick_exception_sink_handle_exception (GumQuickExceptionSink * self,
                                           JSValueConst exception)
{
  JSContext * ctx = self->core->ctx;
  JSValue result;

  result = JS_Call (ctx, self->callback, JS_UNDEFINED, 1, &exception);
  if (JS_IsException (result))
    _gum_quick_panic (ctx, "Error handler crashed");

  JS_FreeValue (ctx, result);
}

static GumQuickMessageSink *
gum_quick_message_sink_new (JSValueConst callback,
                            GumQuickCore * core)
{
  GumQuickMessageSink * sink;

  sink = g_slice_new (GumQuickMessageSink);
  sink->callback = JS_DupValue (core->ctx, callback);
  sink->core = core;

  return sink;
}

static void
gum_quick_message_sink_free (GumQuickMessageSink * sink)
{
  JS_FreeValue (sink->core->ctx, sink->callback);

  g_slice_free (GumQuickMessageSink, sink);
}

static void
gum_quick_message_sink_post (GumQuickMessageSink * self,
                             const gchar * message,
                             GBytes * data,
                             GumQuickScope * scope)
{
  JSContext * ctx = self->core->ctx;
  JSValue argv[2];

  argv[0] = JS_NewString (ctx, message);

  if (data != NULL)
  {
    gpointer data_buffer;
    gsize data_size;

    data_buffer = g_bytes_unref_to_data (data, &data_size);

    argv[1] = JS_NewArrayBuffer (ctx, data_buffer, data_size,
        _gum_quick_array_buffer_free, data_buffer, FALSE);
  }
  else
  {
    argv[1] = JS_NULL;
  }

  _gum_quick_scope_call_void (scope, self->callback, JS_UNDEFINED,
      G_N_ELEMENTS (argv), argv);

  JS_FreeValue (ctx, argv[1]);
  JS_FreeValue (ctx, argv[0]);
}

static gboolean
gum_quick_ffi_type_get (JSContext * ctx,
                        JSValueConst val,
                        GumQuickCore * core,
                        ffi_type ** type,
                        GSList ** data)
{
  gboolean success = FALSE;
  JSValue field_value;

  if (JS_IsString (val))
  {
    const gchar * type_name = JS_ToCString (ctx, val);
    success = gum_ffi_try_get_type_by_name (type_name, type);
    JS_FreeCString (ctx, type_name);
  }
  else if (JS_IsArray (ctx, val))
  {
    guint length, i;
    ffi_type ** fields, * struct_type;

    if (!_gum_quick_array_get_length (ctx, val, core, &length))
      return FALSE;

    fields = g_new (ffi_type *, length + 1);
    *data = g_slist_prepend (*data, fields);

    for (i = 0; i != length; i++)
    {
      field_value = JS_GetPropertyUint32 (ctx, val, i);

      if (!gum_quick_ffi_type_get (ctx, field_value, core, &fields[i], data))
        goto invalid_field_value;

      JS_FreeValue (ctx, field_value);
    }

    fields[length] = NULL;

    struct_type = g_new0 (ffi_type, 1);
    struct_type->type = FFI_TYPE_STRUCT;
    struct_type->elements = fields;
    *data = g_slist_prepend (*data, struct_type);

    *type = struct_type;
    success = TRUE;
  }

  if (!success)
    _gum_quick_throw_literal (ctx, "invalid type specified");

  return success;

invalid_field_value:
  {
    JS_FreeValue (ctx, field_value);

    return FALSE;
  }
}

static gboolean
gum_quick_ffi_abi_get (JSContext * ctx,
                       const gchar * name,
                       ffi_abi * abi)
{
  if (gum_ffi_try_get_abi_by_name (name, abi))
    return TRUE;

  _gum_quick_throw_literal (ctx, "invalid abi specified");
  return FALSE;
}

static gboolean
gum_quick_value_to_ffi (JSContext * ctx,
                        JSValueConst sval,
                        const ffi_type * type,
                        GumQuickCore * core,
                        GumFFIValue * val)
{
  gint i;
  guint u;
  gint64 i64;
  guint64 u64;
  gdouble d;

  if (type == &ffi_type_void)
  {
    val->v_pointer = NULL;
  }
  else if (type == &ffi_type_pointer)
  {
    if (!_gum_quick_native_pointer_get (ctx, sval, core, &val->v_pointer))
      return FALSE;
  }
  else if (type == &ffi_type_sint8)
  {
    if (!_gum_quick_int_get (ctx, sval, &i))
      return FALSE;
    val->v_sint8 = i;
  }
  else if (type == &ffi_type_uint8)
  {
    if (!_gum_quick_uint_get (ctx, sval, &u))
      return FALSE;
    val->v_uint8 = u;
  }
  else if (type == &ffi_type_sint16)
  {
    if (!_gum_quick_int_get (ctx, sval, &i))
      return FALSE;
    val->v_sint16 = i;
  }
  else if (type == &ffi_type_uint16)
  {
    if (!_gum_quick_uint_get (ctx, sval, &u))
      return FALSE;
    val->v_uint16 = u;
  }
  else if (type == &ffi_type_sint32)
  {
    if (!_gum_quick_int_get (ctx, sval, &i))
      return FALSE;
    val->v_sint32 = i;
  }
  else if (type == &ffi_type_uint32)
  {
    if (!_gum_quick_uint_get (ctx, sval, &u))
      return FALSE;
    val->v_uint32 = u;
  }
  else if (type == &ffi_type_sint64)
  {
    if (!_gum_quick_int64_get (ctx, sval, core, &i64))
      return FALSE;
    val->v_sint64 = i64;
  }
  else if (type == &ffi_type_uint64)
  {
    if (!_gum_quick_uint64_get (ctx, sval, core, &u64))
      return FALSE;
    val->v_uint64 = u64;
  }
  else if (type == &ffi_type_float)
  {
    if (!_gum_quick_float64_get (ctx, sval, &d))
      return FALSE;
    val->v_float = d;
  }
  else if (type == &ffi_type_double)
  {
    if (!_gum_quick_float64_get (ctx, sval, &d))
      return FALSE;
    val->v_double = d;
  }
  else if (type->type == FFI_TYPE_STRUCT)
  {
    ffi_type ** const field_types = type->elements, ** t;
    guint length, expected_length, field_index;
    guint8 * field_values;
    gsize offset;

    if (!_gum_quick_array_get_length (ctx, sval, core, &length))
      return FALSE;

    expected_length = 0;
    for (t = field_types; *t != NULL; t++)
      expected_length++;

    if (length != expected_length)
      return FALSE;

    field_values = (guint8 *) val;
    offset = 0;

    for (field_index = 0; field_index != length; field_index++)
    {
      const ffi_type * field_type = field_types[field_index];
      GumFFIValue * field_val;
      JSValue field_sval;
      gboolean valid;

      offset = GUM_ALIGN_SIZE (offset, field_type->alignment);

      field_val = (GumFFIValue *) (field_values + offset);

      field_sval = JS_GetPropertyUint32 (ctx, sval, field_index);
      if (JS_IsException (field_sval))
        return FALSE;

      valid =
          gum_quick_value_to_ffi (ctx, field_sval, field_type, core, field_val);

      JS_FreeValue (ctx, field_sval);

      if (!valid)
        return FALSE;

      offset += field_type->size;
    }
  }
  else
  {
    g_assert_not_reached ();
  }

  return TRUE;
}

static JSValue
gum_quick_value_from_ffi (JSContext * ctx,
                          const GumFFIValue * val,
                          const ffi_type * type,
                          GumQuickCore * core)
{
  if (type == &ffi_type_void)
  {
    return JS_UNDEFINED;
  }
  else if (type == &ffi_type_pointer)
  {
    return _gum_quick_native_pointer_new (ctx, val->v_pointer, core);
  }
  else if (type == &ffi_type_sint8)
  {
    return JS_NewInt32 (ctx, val->v_sint8);
  }
  else if (type == &ffi_type_uint8)
  {
    return JS_NewUint32 (ctx, val->v_uint8);
  }
  else if (type == &ffi_type_sint16)
  {
    return JS_NewInt32 (ctx, val->v_sint16);
  }
  else if (type == &ffi_type_uint16)
  {
    return JS_NewUint32 (ctx, val->v_uint16);
  }
  else if (type == &ffi_type_sint32)
  {
    return JS_NewInt32 (ctx, val->v_sint32);
  }
  else if (type == &ffi_type_uint32)
  {
    return JS_NewUint32 (ctx, val->v_uint32);
  }
  else if (type == &ffi_type_sint64)
  {
    return _gum_quick_int64_new (ctx, val->v_sint64, core);
  }
  else if (type == &ffi_type_uint64)
  {
    return _gum_quick_uint64_new (ctx, val->v_uint64, core);
  }
  else if (type == &ffi_type_float)
  {
    return JS_NewFloat64 (ctx, val->v_float);
  }
  else if (type == &ffi_type_double)
  {
    return JS_NewFloat64 (ctx, val->v_double);
  }
  else if (type->type == FFI_TYPE_STRUCT)
  {
    ffi_type ** const field_types = type->elements, ** t;
    guint length, i;
    const guint8 * field_values;
    gsize offset;
    JSValue field_svalues;

    length = 0;
    for (t = field_types; *t != NULL; t++)
      length++;

    field_values = (const guint8 *) val;
    offset = 0;

    field_svalues = JS_NewArray (ctx);

    for (i = 0; i != length; i++)
    {
      const ffi_type * field_type = field_types[i];
      const GumFFIValue * field_val;
      JSValue field_sval;

      offset = GUM_ALIGN_SIZE (offset, field_type->alignment);
      field_val = (const GumFFIValue *) (field_values + offset);

      field_sval = gum_quick_value_from_ffi (ctx, field_val, field_type, core);

      JS_DefinePropertyValueUint32 (ctx, field_svalues, i, field_sval,
          JS_PROP_C_W_E);

      offset += field_type->size;
    }

    return field_svalues;
  }
  else
  {
    g_assert_not_reached ();
  }
}

static void
gum_quick_core_setup_atoms (GumQuickCore * self)
{
  JSContext * ctx = self->ctx;

#define GUM_SETUP_ATOM(id) \
    GUM_SETUP_ATOM_NAMED (id, G_STRINGIFY (id))
#define GUM_SETUP_ATOM_NAMED(id, name) \
    GUM_QUICK_CORE_ATOM (self, id) = JS_NewAtom (ctx, name)

  GUM_SETUP_ATOM (abi);
  GUM_SETUP_ATOM (address);
  GUM_SETUP_ATOM (autoClose);
  GUM_SETUP_ATOM (base);
  GUM_SETUP_ATOM_NAMED (cachedInput, "$i");
  GUM_SETUP_ATOM_NAMED (cachedOutput, "$o");
  GUM_SETUP_ATOM (context);
  GUM_SETUP_ATOM (exceptions);
  GUM_SETUP_ATOM (file);
  GUM_SETUP_ATOM (handle);
  GUM_SETUP_ATOM (id);
  GUM_SETUP_ATOM (ip);
  GUM_SETUP_ATOM (isGlobal);
  GUM_SETUP_ATOM (length);
  GUM_SETUP_ATOM (memory);
  GUM_SETUP_ATOM (message);
  GUM_SETUP_ATOM (module);
  GUM_SETUP_ATOM (name);
  GUM_SETUP_ATOM (nativeContext);
  GUM_SETUP_ATOM (offset);
  GUM_SETUP_ATOM (operation);
  GUM_SETUP_ATOM (path);
  GUM_SETUP_ATOM (pc);
  GUM_SETUP_ATOM (port);
  GUM_SETUP_ATOM (protection);
  GUM_SETUP_ATOM (prototype);
  GUM_SETUP_ATOM_NAMED (resource, "$r");
  GUM_SETUP_ATOM (scheduling);
  GUM_SETUP_ATOM (section);
  GUM_SETUP_ATOM (size);
  GUM_SETUP_ATOM (slot);
  GUM_SETUP_ATOM (state);
  GUM_SETUP_ATOM_NAMED (system_error, GUMJS_SYSTEM_ERROR_FIELD);
  GUM_SETUP_ATOM (traps);
  GUM_SETUP_ATOM (type);
  GUM_SETUP_ATOM (value);

#if defined (HAVE_I386)
  GUM_SETUP_ATOM (disp);
  GUM_SETUP_ATOM (index);
  GUM_SETUP_ATOM (scale);
  GUM_SETUP_ATOM (segment);
#elif defined (HAVE_ARM)
  GUM_SETUP_ATOM (disp);
  GUM_SETUP_ATOM (index);
  GUM_SETUP_ATOM (scale);
  GUM_SETUP_ATOM (shift);
  GUM_SETUP_ATOM (subtracted);
  GUM_SETUP_ATOM (vectorIndex);
#elif defined (HAVE_ARM64)
  GUM_SETUP_ATOM (disp);
  GUM_SETUP_ATOM (ext);
  GUM_SETUP_ATOM (index);
  GUM_SETUP_ATOM (shift);
  GUM_SETUP_ATOM (vas);
  GUM_SETUP_ATOM (vectorIndex);
#elif defined (HAVE_MIPS)
  GUM_SETUP_ATOM (disp);
#endif

#undef GUM_SETUP_ATOM
}

static void
gum_quick_core_teardown_atoms (GumQuickCore * self)
{
  JSContext * ctx = self->ctx;

#define GUM_TEARDOWN_ATOM(id) \
    JS_FreeAtom (ctx, GUM_QUICK_CORE_ATOM (self, id)); \
    GUM_QUICK_CORE_ATOM (self, id) = JS_ATOM_NULL

  GUM_TEARDOWN_ATOM (abi);
  GUM_TEARDOWN_ATOM (address);
  GUM_TEARDOWN_ATOM (autoClose);
  GUM_TEARDOWN_ATOM (base);
  GUM_TEARDOWN_ATOM (cachedInput);
  GUM_TEARDOWN_ATOM (cachedOutput);
  GUM_TEARDOWN_ATOM (context);
  GUM_TEARDOWN_ATOM (exceptions);
  GUM_TEARDOWN_ATOM (file);
  GUM_TEARDOWN_ATOM (handle);
  GUM_TEARDOWN_ATOM (id);
  GUM_TEARDOWN_ATOM (ip);
  GUM_TEARDOWN_ATOM (isGlobal);
  GUM_TEARDOWN_ATOM (length);
  GUM_TEARDOWN_ATOM (memory);
  GUM_TEARDOWN_ATOM (message);
  GUM_TEARDOWN_ATOM (module);
  GUM_TEARDOWN_ATOM (name);
  GUM_TEARDOWN_ATOM (nativeContext);
  GUM_TEARDOWN_ATOM (offset);
  GUM_TEARDOWN_ATOM (operation);
  GUM_TEARDOWN_ATOM (path);
  GUM_TEARDOWN_ATOM (pc);
  GUM_TEARDOWN_ATOM (port);
  GUM_TEARDOWN_ATOM (protection);
  GUM_TEARDOWN_ATOM (prototype);
  GUM_TEARDOWN_ATOM (resource);
  GUM_TEARDOWN_ATOM (scheduling);
  GUM_TEARDOWN_ATOM (section);
  GUM_TEARDOWN_ATOM (size);
  GUM_TEARDOWN_ATOM (slot);
  GUM_TEARDOWN_ATOM (state);
  GUM_TEARDOWN_ATOM (system_error);
  GUM_TEARDOWN_ATOM (traps);
  GUM_TEARDOWN_ATOM (type);
  GUM_TEARDOWN_ATOM (value);

#if defined (HAVE_I386)
  GUM_TEARDOWN_ATOM (disp);
  GUM_TEARDOWN_ATOM (index);
  GUM_TEARDOWN_ATOM (scale);
  GUM_TEARDOWN_ATOM (segment);
#elif defined (HAVE_ARM)
  GUM_TEARDOWN_ATOM (disp);
  GUM_TEARDOWN_ATOM (index);
  GUM_TEARDOWN_ATOM (scale);
  GUM_TEARDOWN_ATOM (shift);
  GUM_TEARDOWN_ATOM (subtracted);
  GUM_TEARDOWN_ATOM (vectorIndex);
#elif defined (HAVE_ARM64)
  GUM_TEARDOWN_ATOM (disp);
  GUM_TEARDOWN_ATOM (ext);
  GUM_TEARDOWN_ATOM (index);
  GUM_TEARDOWN_ATOM (shift);
  GUM_TEARDOWN_ATOM (vas);
  GUM_TEARDOWN_ATOM (vectorIndex);
#elif defined (HAVE_MIPS)
  GUM_TEARDOWN_ATOM (disp);
#endif

#undef GUM_TEARDOWN_ATOM
}