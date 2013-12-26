/*
 * Copyright © 2014 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */
#include <glib-object.h>

#include <gjs/gjs.h>
#include <gjs/compat.h>
#include <gjs/gjs-module.h>

#include <gjs/debug-hooks.h>
#include <gjs/reflected-script.h>

#include "debug-hooks-private.h"

struct _GjsDebugHooksPrivate {
    /* Non-owning reference to the context */
    GjsContext *context;

    /* Hook usage counts.
     *
     * Each of these counters correspond to a particular function
     * that we have a registered callback for or need for in SpiderMonkey.
     *
     * When someone wants to use the function we increment the count
     * and if they are the first user, set it up to be in the right state.
     * When someone is the last user and no longer wants to use the
     * function, they decrement the count and then do appropriate
     * tear down on the state.
     *
     * There are states that we absolutely do not want to leave enabled
     * longer than we have to, for instance, JS_SetSingleStepMode or
     * JS_SetDebugMode */
    unsigned int debug_mode_usage_count;
    unsigned int single_step_mode_usage_count;
    unsigned int interrupt_function_usage_count;
    unsigned int call_and_execute_hook_usage_count;
    unsigned int new_script_hook_usage_count;

    /* These are data structures which contain callback points
     * whenever our internal JS debugger hooks get called */

    /* Breakpoints are those which have been activated in the context
     * and have a trap set on them. Pending breakpoints are those for
     * scripts that we haven't loaded yet and will be activated as
     * soon as they are loaded. A breakpoint's callback will
     * be triggered if we hit that particular breakpoint */
    GHashTable *breakpoints;
    GHashTable *pending_breakpoints;

    /* Each of these are all called as soon as we single-step
     * one line, enter a new execution frame, or load a new script */
    GArray     *single_step_hooks;
    GArray     *call_and_execute_hooks;
    GArray     *new_script_hooks;

    /* These are data structures which we can use to
     * look up the keys for the above structures on
     * destruction. */
    GHashTable *breakpoints_connections;
    GHashTable *single_step_connections;
    GHashTable *call_and_execute_connections;
    GHashTable *new_script_connections;

    /* This is a hashtable of GjsDebugScriptLookupInfo to
     * JSScript */
    GHashTable *scripts_loaded;

    /* An array of jsbytecode for the stack of program counters
     * as we have entered/exited from our execution hook. We push
     * a new program counter on to the stack every time we enter a
     * new frame and can get this information on frame exit to
     * determine the location of each function in a stack */
    GArray *pc_stack;
};

G_DEFINE_TYPE_WITH_PRIVATE(GjsDebugHooks,
                           gjs_debug_hooks,
                           G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_CONTEXT,
    PROP_N
};

static GParamSpec *properties[PROP_N];

typedef struct _GjsLocationInfo {
    GjsFrameInfo current_frame;

    /* Storage for filenames and function names so
     * that we don't expose non-const memory to
     * users of GjsFunctionKey */
    char *filename;
    char *current_function_name;
} GjsLocationInfo;

const GjsFrameInfo *
gjs_location_info_get_current_frame(const GjsLocationInfo *info)
{
    return &info->current_frame;
}

const char *
gjs_debug_script_info_get_filename(const GjsDebugScriptInfo *info)
{
    return (const char *) info->filename;
}

typedef struct _GjsDebugUserCallback {
    GCallback callback;
    gpointer  user_data;
} GjsDebugUserCallback;

static void gjs_debug_hooks_finish_using_new_script_callback(GjsDebugHooks *hooks);

static void
gjs_debug_user_callback_assign(GjsDebugUserCallback *user_callback,
                               GCallback             callback,
                               gpointer              user_data)
{
    user_callback->callback = callback;
    user_callback->user_data = user_data;
}

static GjsDebugUserCallback *
gjs_debug_user_callback_new(GCallback callback,
                            gpointer  user_data)
{
    GjsDebugUserCallback *user_callback = g_new0(GjsDebugUserCallback, 1);
    gjs_debug_user_callback_assign(user_callback, callback, user_data);
    return user_callback;
}

static void
gjs_debug_user_callback_free(GjsDebugUserCallback *user_callback)
{
    g_free(user_callback);
}

typedef struct _GjsDebugScriptLookupInfo
{
    char         *name;
    unsigned int lineno;
} GjsDebugScriptLookupInfo;

static GjsDebugScriptLookupInfo *
gjs_debug_script_lookup_info_new(const char   *name,
                                 unsigned int  lineno)
{
    GjsDebugScriptLookupInfo *info = g_new0(GjsDebugScriptLookupInfo, 1);
    info->name = g_strdup(name);
    info->lineno = lineno;
    return info;
}

static unsigned int
gjs_debug_script_lookup_info_hash(gconstpointer key)
{
    GjsDebugScriptLookupInfo *info = (GjsDebugScriptLookupInfo *) key;
    return g_int_hash(&info->lineno) ^ g_str_hash(info->name);
}

static gboolean
gjs_debug_script_lookup_info_equal(gconstpointer first,
                                   gconstpointer second)
{
    GjsDebugScriptLookupInfo *first_info = (GjsDebugScriptLookupInfo *) first;
    GjsDebugScriptLookupInfo *second_info = (GjsDebugScriptLookupInfo *) second;

    return first_info->lineno == second_info->lineno &&
           g_strcmp0(first_info->name, second_info->name) == 0;
}

static void
gjs_debug_script_lookup_info_destroy(gpointer info)
{
    GjsDebugScriptLookupInfo *lookup_info = (GjsDebugScriptLookupInfo *) info;
    g_free(lookup_info->name);
    g_free(lookup_info);
}

typedef struct _InterruptCallbackDispatchData {
    GjsDebugHooks   *hooks;
    GjsLocationInfo *info;
} InterruptCallbackDispatchData;

static void
dispatch_interrupt_callback(gpointer element,
                            gpointer user_data)
{
    GjsDebugUserCallback          *user_callback = (GjsDebugUserCallback *) element;
    InterruptCallbackDispatchData *dispatch_data = (InterruptCallbackDispatchData *) user_data;
    GjsDebugHooksPrivate          *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                    *context = priv->context;
    GjsInterruptCallback           callback = (GjsInterruptCallback) user_callback->callback;

    callback(dispatch_data->hooks,
             context,
             dispatch_data->info,
             user_callback->user_data);
}

typedef struct _InfoCallbackDispatchData {
    GjsDebugHooks      *hooks;
    GjsDebugScriptInfo *info;
} InfoCallbackDispatchData;

static void
dispatch_info_callback(gpointer element,
                       gpointer user_data)

{
    GjsDebugUserCallback            *user_callback = (GjsDebugUserCallback *) element;
    InfoCallbackDispatchData        *dispatch_data = (InfoCallbackDispatchData *) user_data;
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                      *context = priv->context;
    GjsInfoCallback                 callback = (GjsInfoCallback) user_callback->callback;

    callback(dispatch_data->hooks,
             context,
             dispatch_data->info,
             user_callback->user_data);
}

typedef struct _FrameCallbackDispatchData {
  GjsDebugHooks    *hooks;
  GjsLocationInfo  *info;
  GjsFrameState    state;
} FrameCallbackDispatchData;

static void
dispatch_frame_callbacks(gpointer element,
                         gpointer user_data)

{
    GjsDebugUserCallback      *user_callback = (GjsDebugUserCallback *) element;
    FrameCallbackDispatchData *dispatch_data = (FrameCallbackDispatchData *) user_data;
    GjsDebugHooksPrivate      *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(dispatch_data->hooks);
    GjsContext                *context = priv->context;
    GjsFrameCallback          callback = (GjsFrameCallback) user_callback->callback;

    callback(dispatch_data->hooks,
             context,
             dispatch_data->info,
             dispatch_data->state,
             user_callback->user_data);
}

static void
for_each_element_in_array(GArray   *array,
                          GFunc     func,
                          gpointer  user_data)
{
    const gsize element_size = g_array_get_element_size(array);
    unsigned int i;
    char         *current_array_pointer = (char *) array->data;

    for (i = 0; i < array->len; ++i, current_array_pointer += element_size)
        (*func)(current_array_pointer, user_data);
}

static char *
get_fully_qualified_path(const char *filename)
{
    /* If this "filename" is actually a URI then just strip the URI
     * header and return a copy of the string */
    char *potential_uri = g_uri_parse_scheme(filename);
    if (potential_uri) {
        g_free(potential_uri);
        return g_strdup(filename);
    }

    char *fully_qualified_path = NULL;
    /* Sometimes we might get just a basename if the script is in the current
     * working directly. If that's the case, then we need to add the fully
     * qualified pathname */
    if (!g_path_is_absolute(filename)) {
        char *current_dir = g_get_current_dir();
        fully_qualified_path = g_strconcat(current_dir,
                                           "/",
                                           filename,
                                           NULL);
        g_free(current_dir);
    } else {
        fully_qualified_path = g_strdup(filename);
    }

    return fully_qualified_path;
}

static void
populate_frame_info_for_location_info_and_pc (GjsLocationInfo *info,
                                              GjsFrameInfo    *frame,
                                              JSContext       *context,
                                              JSScript        *script,
                                              JSFunction      *function,
                                              jsbytecode      *pc,
                                              jsbytecode      *current_function_pc)
{
    frame->current_function.filename = info->filename;
    frame->current_function.function_name = info->current_function_name;
    frame->current_function.line = JS_PCToLineNumber(context,
                                                     script,
                                                     current_function_pc);

    if (function)
        frame->current_function.n_args = JS_GetFunctionArgumentCount(context, function);
    else
        frame->current_function.n_args = 0;

    frame->current_line = JS_PCToLineNumber(context, script, pc);
}

static void
populate_location_info_from_js_function(GjsLocationInfo *info,
                                        JSContext       *js_context,
                                        JSScript        *script,
                                        JSFunction      *js_function)
{
    JSString *js_function_name = NULL;

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    info->filename = get_fully_qualified_path(JS_GetScriptFilename(js_context, script));

    char *function_name = NULL;

    /* Only set the function name if we're actually in a function */
    if (js_function)
    {
        js_function_name = JS_GetFunctionId(js_function);

        if (!js_function_name) {
            function_name = g_strdup_printf("(anonymous)");
        } else {
            if (!gjs_string_to_utf8(js_context,
                                    STRING_TO_JSVAL(js_function_name),
                                    &function_name))
                gjs_throw(js_context, "Failed to convert function name to utf8 string!");
        }
    }

    info->current_function_name = function_name;
}

static void
gjs_debug_hooks_populate_location_info(GjsLocationInfo *info,
                                       JSContext       *js_context,
                                       JSScript        *script,
                                       jsbytecode      *current_line_pc,
                                       jsbytecode      *current_function_pc)
{
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    JSFunction *js_function  = JS_GetScriptFunction(js_context, script);
    populate_location_info_from_js_function(info, js_context, script, js_function);
    populate_frame_info_for_location_info_and_pc(info,
                                                 &info->current_frame,
                                                 js_context,
                                                 script,
                                                 js_function,
                                                 current_line_pc,
                                                 current_function_pc);
}

static void
gjs_debug_hooks_clear_location_info(GjsLocationInfo *info)
{
    g_free(info->current_function_name);
    g_free(info->filename);
}

static void
gjs_debug_hooks_populate_script_info(GjsDebugScriptInfo *info,
                                     JSContext          *js_context,
                                     JSScript           *script,
                                     const char         *filename)
{
    info->filename = filename;
    info->begin_line = JS_GetScriptBaseLineNumber(js_context, script);
}

typedef struct _GjsBreakpoint {
    JSScript *script;
    jsbytecode *pc;
} GjsBreakpoint;

static void
gjs_breakpoint_destroy(gpointer data)
{
    GjsBreakpoint *breakpoint = (GjsBreakpoint *) data;

    g_free(breakpoint);
}

static GjsBreakpoint *
gjs_breakpoint_new(JSScript   *script,
                   jsbytecode *pc)
{
    GjsBreakpoint *breakpoint = g_new0(GjsBreakpoint, 1);
    breakpoint->script = script;
    breakpoint->pc = pc;
    return breakpoint;
}

typedef struct _GjsPendingBreakpoint {
    char         *filename;
    unsigned int lineno;
} GjsPendingBreakpoint;

static void
gjs_pending_breakpoint_destroy(gpointer data)
{
    GjsPendingBreakpoint *pending = (GjsPendingBreakpoint *) data;
    g_free(pending->filename);
    g_free(pending);
}

static GjsPendingBreakpoint *
gjs_pending_breakpoint_new(const char   *filename,
                           unsigned int  lineno)
{
    GjsPendingBreakpoint *pending = g_new0(GjsPendingBreakpoint, 1);
    pending->filename = g_strdup(filename);
    pending->lineno = lineno;
    return pending;
}

typedef struct _BreakpointActivationData {
    GjsDebugHooks *debug_hooks;
    JSContext                *js_context;
    JSScript                 *js_script;
    const char               *filename;
    unsigned int             begin_lineno;
    GHashTable               *breakpoints;
    GList                    *breakpoints_changed;
} BreakpointActivationData;

static void
remove_breakpoint_from_hashtable_by_user_callback(gpointer list_item,
                                                  gpointer hashtable_pointer)
{
    GHashTable           *breakpoints = (GHashTable *) hashtable_pointer;
    GjsPendingBreakpoint *pending_breakpoint = (GjsPendingBreakpoint *) g_hash_table_lookup(breakpoints, list_item);

    g_return_if_fail(pending_breakpoint);

    gjs_pending_breakpoint_destroy(pending_breakpoint);
    g_hash_table_remove(breakpoints, list_item);
}

static void
remove_activated_breakpoints_from_pending(GList      *activated_breakpoints,
                                          GHashTable *pending)
{
    g_list_foreach(activated_breakpoints,
                   remove_breakpoint_from_hashtable_by_user_callback,
                   pending);
    g_list_free(activated_breakpoints);
}

static unsigned int
get_script_end_lineno(JSContext *js_context,
                      JSScript  *js_script)
{
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    jsbytecode *pc = JS_EndPC(js_context, js_script);
    return JS_PCToLineNumber(js_context,
                             js_script,
                             pc);
}

typedef struct _GjsMultiplexedDebugHooksTrapPrivateData {
    GjsDebugHooks *hooks;
    GjsDebugUserCallback     *user_callback;
} GjsMultiplexedDebugHooksTrapPrivateData;

GjsMultiplexedDebugHooksTrapPrivateData *
gjs_debug_hooks_trap_private_data_new(GjsDebugHooks        *hooks,
                                      GjsDebugUserCallback *user_callback)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        g_new0(GjsMultiplexedDebugHooksTrapPrivateData, 1);

    data->hooks = hooks;
    data->user_callback = user_callback;

    return data;
}

static void
gjs_debug_hooks_trap_private_data_destroy(GjsMultiplexedDebugHooksTrapPrivateData *data)
{
    g_free (data);
}

static jsbytecode *
tail_for_pc_stack(GArray *pc_stack)
{
    return g_array_index(pc_stack, jsbytecode *, pc_stack->len - 1);
}

/* Callbacks */
static JSTrapStatus
gjs_debug_hooks_trap_handler(JSContext  *context,
                             JSScript   *script,
                             jsbytecode *pc,
                             jsval      *rval,
                             jsval       closure)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        (GjsMultiplexedDebugHooksTrapPrivateData *) JSVAL_TO_PRIVATE(closure);

    /* And there goes the law of demeter */
    GjsDebugHooks *hooks = data->hooks;
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    GjsLocationInfo location_info;
    GjsInterruptCallback callback = (GjsInterruptCallback) data->user_callback->callback;
    gjs_debug_hooks_populate_location_info(&location_info,
                                           context,
                                           script,
                                           pc,
                                           tail_for_pc_stack(priv->pc_stack));

    callback(hooks,
             priv->context,
             &location_info,
             data->user_callback->user_data);

    gjs_debug_hooks_clear_location_info(&location_info);

    return JSTRAP_CONTINUE;
}

static GjsBreakpoint *
gjs_debug_create_native_breakpoint_for_script(GjsDebugHooks        *hooks,
                                              JSContext            *js_context,
                                              JSScript             *script,
                                              unsigned int          line,
                                              GjsDebugUserCallback *user_callback)
{
    GjsMultiplexedDebugHooksTrapPrivateData *data =
        gjs_debug_hooks_trap_private_data_new(hooks, user_callback);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    /* This always succeeds, although it might only return the very-end
     * or very-beginning program counter if the line is out of range */
    jsbytecode *pc =
        JS_LineNumberToPC(js_context, script, line);

    /* Set the breakpoint on the JS side now that we're tracking it */
    JS_SetTrap(js_context,
               script,
               pc,
               gjs_debug_hooks_trap_handler,
               PRIVATE_TO_JSVAL(data));

    return gjs_breakpoint_new(script, pc);
}

static GjsBreakpoint *
create_native_breakpoint_if_within_script(GjsDebugHooks        *debug_hooks,
                                          JSContext            *context,
                                          JSScript             *script,
                                          GjsDebugUserCallback *user_callback,
                                          GjsPendingBreakpoint *pending,
                                          const char           *filename,
                                          unsigned int          begin_lineno)

{
    /* Interrogate the script for its last program counter and thus its
     * last line. If the desired breakpoint line falls within this script's
     * line range then activate it. */
    if (strcmp(filename, pending->filename) == 0) {
        unsigned int end_lineno = get_script_end_lineno(context, script);

        if (begin_lineno <= pending->lineno &&
            end_lineno >= pending->lineno) {
            GjsBreakpoint *breakpoint =
                gjs_debug_create_native_breakpoint_for_script(debug_hooks,
                                                              context,
                                                              script,
                                                              pending->lineno,
                                                              user_callback);

            return breakpoint;
        }
    }

    return NULL;
}

static void
gjs_debug_hooks_new_script_callback(JSContext    *context,
                                    const char  *filename,
                                    unsigned int  lineno,
                                    JSScript     *script,
                                    JSFunction   *function,
                                    gpointer      caller_data)
{
    /* We don't care about NULL-filename scripts, they are probably just initialization
     * scripts */
    if (!filename)
        return;

    GjsDebugHooks *hooks = GJS_DEBUG_HOOKS(caller_data);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    GjsDebugScriptLookupInfo *info =
        gjs_debug_script_lookup_info_new(filename, lineno);

    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    char *fully_qualified_path = get_fully_qualified_path(filename);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    g_hash_table_insert(priv->scripts_loaded,
                        info,
                        script);

    /* Special case - if single-step mode is enabled then we should enable it
     * here */
    if (priv->single_step_mode_usage_count)
        JS_SetSingleStepMode(js_context, script, TRUE);

    /* Special case - search pending breakpoints for the current script filename
     * and convert them to real breakpoints if need be */
    GHashTableIter iter;
    gpointer       key, value;

    GList *breakpoints_changed = NULL;

    g_hash_table_iter_init(&iter, priv->pending_breakpoints);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GjsDebugUserCallback *user_callback = (GjsDebugUserCallback *) key;
        GjsPendingBreakpoint *pending = (GjsPendingBreakpoint *) value;
        GjsBreakpoint        *breakpoint =
            create_native_breakpoint_if_within_script(hooks,
                                                      js_context,
                                                      script,
                                                      user_callback,
                                                      pending,
                                                      fully_qualified_path,
                                                      lineno);

        if (breakpoint) {
            g_hash_table_insert(priv->breakpoints,
                                user_callback,
                                breakpoint);

            /* We append user_callback here as that is what we need to remove-by later */
            breakpoints_changed =
                g_list_append(breakpoints_changed,
                              user_callback);

            /* Decrement new script callback, we might not need to know about
             * new scripts anymore as the breakpoint is no longer pending */
            gjs_debug_hooks_finish_using_new_script_callback(hooks);
        }
    }

    remove_activated_breakpoints_from_pending(breakpoints_changed,
                                              priv->pending_breakpoints);

    GjsDebugScriptInfo debug_script_info;
    gjs_debug_hooks_populate_script_info(&debug_script_info,
                                         context,
                                         script,
                                         fully_qualified_path);


    InfoCallbackDispatchData data = {
        hooks,
        &debug_script_info
    };

    /* Finally, call the callback function */
    for_each_element_in_array(priv->new_script_hooks,
                              dispatch_info_callback,
                              &data);

    g_free(fully_qualified_path);
}

static void
gjs_debug_hooks_script_destroyed_callback(JSFreeOp     *fo,
                                          JSScript     *script,
                                          gpointer      caller_data)
{
    GjsDebugHooks                   *hooks = GJS_DEBUG_HOOKS(caller_data);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);

    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    GjsDebugScriptLookupInfo info = {
        (char *) JS_GetScriptFilename(js_context, script),
        JS_GetScriptBaseLineNumber(js_context, script)
    };

    g_hash_table_remove(priv->scripts_loaded, &info);
}

static JSTrapStatus
gjs_debug_hooks_interrupt_callback(JSContext  *context,
                                   JSScript   *script,
                                   jsbytecode *pc,
                                   jsval      *rval,
                                   gpointer    closure)
{
    GjsDebugHooks                   *hooks = GJS_DEBUG_HOOKS(closure);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    GjsLocationInfo location_info;
    gjs_debug_hooks_populate_location_info(&location_info,
                                           context,
                                           script,
                                           pc,
                                           tail_for_pc_stack(priv->pc_stack));

    InterruptCallbackDispatchData data = {
        hooks,
        &location_info,
    };

    for_each_element_in_array(priv->single_step_hooks,
                              dispatch_interrupt_callback,
                              &data);

    gjs_debug_hooks_clear_location_info(&location_info);

    return JSTRAP_CONTINUE;
}

static void *
gjs_debug_hooks_frame_step_callback(JSContext          *context,
                                    JSAbstractFramePtr  frame,
                                    bool                is_constructing,
                                    JSBool              before,
                                    JSBool             *ok,
                                    gpointer            closure)
{
    JSFunction           *function = frame.maybeFun();
    JSScript             *script = frame.script();
    GjsDebugHooks        *hooks = GJS_DEBUG_HOOKS(closure);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    JSBrokenFrameIterator it(context);
    jsbytecode *current_pc = it.pc();
    jsbytecode *stack_frame_pc = NULL;

    /* If we are entering a new stack frame, then push the current
     * program counter on to our array. The tail of the array will
     * always be the line number of the frame that we're in */
    if (before) {
        g_array_append_val(priv->pc_stack, current_pc);
        stack_frame_pc = current_pc;
    } else {
        stack_frame_pc = tail_for_pc_stack (priv->pc_stack);
        g_array_set_size(priv->pc_stack, priv->pc_stack->len - 1);
    }

    GjsLocationInfo info;
    gjs_debug_hooks_populate_location_info(&info,
                                           context,
                                           script,
                                           current_pc,
                                           current_pc);

    FrameCallbackDispatchData data = {
        hooks,
        &info,
        before ? GJS_FRAME_ENTRY : GJS_FRAME_EXIT
    };

    for_each_element_in_array(priv->call_and_execute_hooks,
                              dispatch_frame_callbacks,
                              &data);

    gjs_debug_hooks_clear_location_info(&info);

    return closure;
}

static void
change_debug_mode(GjsDebugHooks *hooks,
                  unsigned int   flags,
                  gboolean       enabled)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    JSContext *context = (JSContext *) gjs_context_get_native_context(priv->context);
    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_BeginRequest(context);
    JS_SetOptions(context, flags);
    JS_SetDebugMode(context, enabled);
    JS_EndRequest(context);
}

static void
gjs_debug_hooks_use_debug_mode(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (priv->debug_mode_usage_count++ == 0) {
        change_debug_mode(hooks,
                          JSOPTION_BASELINE | JSOPTION_TYPE_INFERENCE,
                          TRUE);
    }
}

static void
gjs_debug_hooks_finish_using_debug_mode(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (--priv->debug_mode_usage_count == 0) {
        change_debug_mode(hooks,
                          0,
                          FALSE);
    }
}

static void
set_interrupt_function_hook(JSContext       *context,
                            JSInterruptHook  callback,
                            gpointer         callback_user_data)
{
    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_SetInterrupt(JS_GetRuntime(context),
                    callback,
                    callback_user_data);
}

static void
gjs_debug_hooks_use_interrupt_function(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (priv->interrupt_function_usage_count++ == 0) {
        set_interrupt_function_hook((JSContext *) gjs_context_get_native_context(priv->context),
                                    gjs_debug_hooks_interrupt_callback,
                                    hooks);
    }
}

static void
gjs_debug_hooks_finish_using_interrupt_function(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (--priv->interrupt_function_usage_count == 0) {
        set_interrupt_function_hook((JSContext *) gjs_context_get_native_context(priv->context),
                                    NULL,
                                    NULL);
    }
}

static void
set_new_script_hook(JSContext           *context,
                    JSNewScriptHook      new_callback,
                    JSDestroyScriptHook  destroy_callback,
                    gpointer             callback_user_data)
{
    JSAutoCompartment ac(context,
                        JS_GetGlobalObject(context));

    JS_SetNewScriptHook(JS_GetRuntime(context), new_callback, callback_user_data);
    JS_SetDestroyScriptHook(JS_GetRuntime(context), destroy_callback, callback_user_data);
}

static void
gjs_debug_hooks_use_new_script_callback(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (priv->new_script_hook_usage_count++ == 0) {
        set_new_script_hook((JSContext *) gjs_context_get_native_context(priv->context),
                            gjs_debug_hooks_new_script_callback,
                            gjs_debug_hooks_script_destroyed_callback,
                            hooks);
    }
}

static void
gjs_debug_hooks_finish_using_new_script_callback(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (--priv->new_script_hook_usage_count == 0) {
        set_new_script_hook((JSContext *) gjs_context_get_native_context(priv->context),
                            NULL,
                            NULL,
                            NULL);
    }
}

static void
set_single_step_mode_on_registered_script(JSScript  *script,
                                          JSContext *context,
                                          gboolean   enabled)
{
    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_SetSingleStepMode(context,
                         script,
                         enabled);
}

static void
set_single_step_mode(JSContext  *context,
                     GHashTable *scripts,
                     gboolean    enabled)
{
    GHashTableIter iter;
    gpointer       key, value;

    g_hash_table_iter_init(&iter, scripts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        JSScript *script = (JSScript *) value;
        set_single_step_mode_on_registered_script(script, context, enabled);
    }
}

static void
gjs_debug_hooks_use_single_step_mode(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (priv->single_step_mode_usage_count++ == 0) {
        set_single_step_mode((JSContext *) gjs_context_get_native_context(priv->context),
                             priv->scripts_loaded,
                             TRUE);
    }
}

static void
gjs_debug_hooks_finish_using_single_step_mode(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (--priv->single_step_mode_usage_count == 0) {
        set_single_step_mode((JSContext *) gjs_context_get_native_context(priv->context),
                             priv->scripts_loaded,
                             FALSE);
    }
}

static void
set_frame_execution_hooks(GjsDebugHooks     *hooks,
                          JSContext         *context,
                          JSInterpreterHook  hook,
                          gpointer           hook_user_data)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    JSRuntime *js_runtime = JS_GetRuntime(context);

    JSAutoCompartment ac(context,
                         JS_GetGlobalObject(context));

    JS_SetExecuteHook(js_runtime, hook, hook_user_data);
    JS_SetCallHook(js_runtime, hook, hook_user_data);

    /* Make sure to clear the current stack of program
     * counters either way */
    g_array_set_size(priv->pc_stack, 0);
}

static void
gjs_debug_hooks_use_frame_execution(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (priv->call_and_execute_hook_usage_count++ == 0) {
        set_frame_execution_hooks(hooks,
                                  (JSContext *) gjs_context_get_native_context(priv->context),
                                  gjs_debug_hooks_frame_step_callback,
                                  hooks);
    }
}

static void
gjs_debug_hooks_finish_using_frame_execution(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    if (--priv->call_and_execute_hook_usage_count == 0) {
        set_frame_execution_hooks(hooks,
                                  (JSContext *) gjs_context_get_native_context(priv->context),
                                  NULL,
                                  NULL);
    }
}

void
gjs_debug_hooks_remove_breakpoint(GjsDebugHooks *hooks,
                                  guint          handle)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    GjsDebugUserCallback *callback =
        (GjsDebugUserCallback *) g_hash_table_lookup(priv->breakpoints_connections,
                                                     GINT_TO_POINTER(handle));
    GjsBreakpoint *breakpoint =
        (GjsBreakpoint *) g_hash_table_lookup(priv->breakpoints, callback);

    gboolean item_was_removed = FALSE;

    /* Remove breakpoint if there was one */
    if (breakpoint) {
        g_hash_table_remove(priv->breakpoints, callback);

        JSAutoCompartment ac(js_context,
                           JS_GetGlobalObject(js_context));

        jsval previous_closure;

        JS_ClearTrap(js_context,
                     breakpoint->script,
                     breakpoint->pc,
                     NULL,
                     &previous_closure);

        GjsMultiplexedDebugHooksTrapPrivateData *private_data =
            (GjsMultiplexedDebugHooksTrapPrivateData *) JSVAL_TO_PRIVATE(previous_closure);
        gjs_debug_hooks_trap_private_data_destroy(private_data);

        gjs_breakpoint_destroy(breakpoint);
        item_was_removed = TRUE;
    } else {
        /* Try to find pending breakpoints we never got to insert */
        GjsPendingBreakpoint *pending_breakpoint =
            (GjsPendingBreakpoint *) g_hash_table_lookup(priv->pending_breakpoints, callback);

        if (pending_breakpoint) {
            g_hash_table_remove(priv->pending_breakpoints, callback);
            gjs_pending_breakpoint_destroy(pending_breakpoint);

            /* When removing a pending breakpoint, we must also finish using the new
             * script hook as we might not care about new scripts anymore if pending
             * breakpoints are empty */
            gjs_debug_hooks_finish_using_new_script_callback(hooks);

            item_was_removed = TRUE;
        }
    }

    g_assert(item_was_removed);

    g_hash_table_remove(priv->breakpoints_connections, GINT_TO_POINTER(handle));
    gjs_debug_user_callback_free(callback);

    gjs_debug_hooks_finish_using_frame_execution(hooks);
    gjs_debug_hooks_finish_using_debug_mode(hooks);
}

/* Search for a script which has the closest start line to our requested line number */
static JSScript *
lookup_script_for_filename_with_closest_start_line(GjsDebugHooks *hooks,
                                                   const char    *filename,
                                                   unsigned int   line)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    JSContext      *js_context = (JSContext *) gjs_context_get_native_context(priv->context);
    GHashTableIter hash_table_iterator;
    gpointer       key = NULL;
    gpointer       value = NULL;

    g_hash_table_iter_init(&hash_table_iterator, priv->scripts_loaded);

    while (g_hash_table_iter_next(&hash_table_iterator, &key, &value)) {
        GjsDebugScriptLookupInfo *info = (GjsDebugScriptLookupInfo *) key;

        if (g_strcmp0(info->name, filename) == 0) {
            JSScript     *script = (JSScript *) value;
            unsigned int script_end_line = get_script_end_lineno(js_context,
                                                                 script);

            if (info->lineno <= line &&
                script_end_line >= line)
                return script;
        }
    }

    return NULL;
}

static GjsBreakpoint *
lookup_line_and_create_native_breakpoint(JSContext            *js_context,
                                         GjsDebugHooks        *debug_hooks,
                                         const char           *filename,
                                         unsigned int          line,
                                         GjsDebugUserCallback *user_callback)
{
    JSScript *script =
        lookup_script_for_filename_with_closest_start_line(debug_hooks,
                                                           filename,
                                                           line);

    if (!script)
        return NULL;

    return gjs_debug_create_native_breakpoint_for_script(debug_hooks,
                                                         js_context,
                                                         script,
                                                         line,
                                                         user_callback);
}

guint
gjs_debug_hooks_add_breakpoint(GjsDebugHooks        *hooks,
                               const char           *filename,
                               unsigned int          line,
                               GjsInterruptCallback  callback,
                               gpointer              user_data)
{
    static guint debug_hooks_counter = 0;

    GjsDebugHooks        *debug_hooks = GJS_DEBUG_HOOKS(hooks);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(debug_hooks);

    JSContext *js_context =
        (JSContext *) gjs_context_get_native_context(priv->context);

    /* We always have a user callback even if we couldn't successfully create a native
     * breakpoint as we can always fall back to creating a pending one */
    GjsDebugUserCallback *user_callback = gjs_debug_user_callback_new(G_CALLBACK (callback),
                                                                      user_data);
    guint connection = ++debug_hooks_counter;

    /* Try to create a native breakpoint. If it succeeds, add it to the breakpoints
     * table, otherwise create a pending breakpoint */
    GjsBreakpoint *breakpoint = lookup_line_and_create_native_breakpoint(js_context,
                                                                         debug_hooks,
                                                                         filename,
                                                                         line,
                                                                         user_callback);

    if (breakpoint) {
        g_hash_table_insert(priv->breakpoints,
                            user_callback,
                            breakpoint);
    } else {
        GjsPendingBreakpoint *pending = gjs_pending_breakpoint_new(filename, line);
        g_hash_table_insert(priv->pending_breakpoints,
                            user_callback,
                            pending);

        /* We'll need to know about new scripts being loaded too */
        gjs_debug_hooks_use_new_script_callback(debug_hooks);
    }

    g_hash_table_insert(priv->breakpoints_connections,
                        GINT_TO_POINTER(connection),
                        user_callback);

    /* We need debug mode for now */
    gjs_debug_hooks_use_debug_mode(debug_hooks);
    gjs_debug_hooks_use_frame_execution(debug_hooks);

    return connection;
}

static int
lookup_index_by_data_in_array(GArray   *array,
                              gpointer  data)
{
    unsigned int i;
    gsize element_size = g_array_get_element_size(array);
    char *underlying_array_pointer = (char *) array->data;

    for (i = 0, underlying_array_pointer = (char *) array->data;
         i < array->len;
         ++i, underlying_array_pointer += element_size) {
        if (data == (gpointer) underlying_array_pointer)
            return (int) i;
    }

    return -1;
}

static guint
insert_callback_for_hook(GArray     *hooks_array,
                         GHashTable *hooks_connections_table,
                         GCallback   callback,
                         gpointer    user_data)
{
    static guint callbacks_hooks_counter = 0;

    unsigned int last_size = hooks_array->len;
    g_array_set_size(hooks_array,
                     last_size + 1);

    GjsDebugUserCallback *user_callback =
        &(g_array_index(hooks_array,
                        GjsDebugUserCallback,
                        last_size));

    gjs_debug_user_callback_assign(user_callback,
                                    callback,
                                    user_data);

    guint connection = ++callbacks_hooks_counter;

    g_hash_table_insert(hooks_connections_table,
                        GINT_TO_POINTER(connection),
                        user_callback);

    return connection;
}

static void
remove_callback_for_hook(guint       connection,
                         GHashTable *hooks_connection_table,
                         GArray     *hooks_array)
{
    GjsDebugUserCallback *user_callback =
        (GjsDebugUserCallback *) g_hash_table_lookup(hooks_connection_table,
                                                     GINT_TO_POINTER(connection));
    int array_index = lookup_index_by_data_in_array(hooks_array,
                                                    user_callback);

    g_hash_table_remove(hooks_connection_table,
                        GINT_TO_POINTER(connection));

    if (array_index > -1)
        g_array_remove_index(hooks_array, array_index);
    else
        g_error("Unable to find user callback %p in array index!", user_callback);
}

void
gjs_debug_hooks_remove_singlestep_hook(GjsDebugHooks *hooks,
                                       guint          connection)
{
    GjsDebugHooks        *multiplexed_hooks = GJS_DEBUG_HOOKS(hooks);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(multiplexed_hooks);
    remove_callback_for_hook(connection,
                             priv->single_step_connections,
                             priv->single_step_hooks);

    gjs_debug_hooks_finish_using_frame_execution(multiplexed_hooks);
    gjs_debug_hooks_finish_using_interrupt_function(multiplexed_hooks);
    gjs_debug_hooks_finish_using_single_step_mode(multiplexed_hooks);
    gjs_debug_hooks_finish_using_new_script_callback(hooks);
    gjs_debug_hooks_finish_using_debug_mode(multiplexed_hooks);
}

guint
gjs_debug_hooks_add_singlestep_hook(GjsDebugHooks        *hooks,
                                    GjsInterruptCallback  callback,
                                    gpointer              user_data)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    gjs_debug_hooks_use_debug_mode(hooks);
    gjs_debug_hooks_use_interrupt_function(hooks);
    gjs_debug_hooks_use_single_step_mode(hooks);
    gjs_debug_hooks_use_frame_execution(hooks);
    gjs_debug_hooks_use_new_script_callback(hooks);
    return insert_callback_for_hook(priv->single_step_hooks,
                                    priv->single_step_connections,
                                    G_CALLBACK(callback),
                                    user_data);
}

void
gjs_debug_hooks_remove_script_load_hook(GjsDebugHooks *hooks,
                                        guint          connection)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    remove_callback_for_hook(connection,
                             priv->new_script_connections,
                             priv->new_script_hooks);
    gjs_debug_hooks_finish_using_new_script_callback(hooks);
    gjs_debug_hooks_finish_using_debug_mode(hooks);
}

guint
gjs_debug_hooks_add_script_load_hook(GjsDebugHooks   *hooks,
                                     GjsInfoCallback  callback,
                                     gpointer         user_data)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    gjs_debug_hooks_use_debug_mode(hooks);
    gjs_debug_hooks_use_new_script_callback(hooks);
    return insert_callback_for_hook(priv->new_script_hooks,
                                    priv->new_script_connections,
                                    G_CALLBACK(callback),
                                    user_data);
}

void
gjs_debug_hooks_remove_frame_step_hook(GjsDebugHooks *hooks,
                                       guint          connection)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    remove_callback_for_hook(connection,
                             priv->call_and_execute_connections,
                             priv->call_and_execute_hooks);
    gjs_debug_hooks_finish_using_frame_execution(hooks);
    gjs_debug_hooks_finish_using_debug_mode(hooks);
}

guint
gjs_debug_hooks_add_frame_step_hook(GjsDebugHooks    *hooks,
                                    GjsFrameCallback  callback,
                                    gpointer          user_data)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);
    gjs_debug_hooks_use_debug_mode(hooks);
    gjs_debug_hooks_use_frame_execution(hooks);
    return insert_callback_for_hook(priv->call_and_execute_hooks,
                                    priv->call_and_execute_connections,
                                    G_CALLBACK(callback),
                                    user_data);
}

static void
gjs_debug_hooks_init(GjsDebugHooks *hooks)
{
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    priv->scripts_loaded = g_hash_table_new_full(gjs_debug_script_lookup_info_hash,
                                                 gjs_debug_script_lookup_info_equal,
                                                 gjs_debug_script_lookup_info_destroy,
                                                 NULL);

    priv->breakpoints_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->new_script_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->call_and_execute_connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->single_step_connections = g_hash_table_new(g_direct_hash, g_direct_equal);

    priv->breakpoints = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->pending_breakpoints = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->single_step_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
    priv->call_and_execute_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
    priv->new_script_hooks = g_array_new(TRUE, TRUE, sizeof(GjsDebugUserCallback));
    priv->pc_stack = g_array_new(FALSE, TRUE, sizeof(jsbytecode *));
}

static void
unref_all_hashtables(GHashTable **hashtable_array)
{
    GHashTable **hashtable_iterator = hashtable_array;

    do {
        g_assert (g_hash_table_size(*hashtable_iterator) == 0);
        g_hash_table_unref(*hashtable_iterator);
    } while (*(++hashtable_iterator));
}

static void
destroy_all_arrays(GArray **array_array)
{
    GArray **array_iterator = array_array;

    do {
        g_assert((*array_iterator)->len == 0);
        g_array_free(*array_iterator, TRUE);
    } while (*(++array_iterator));
}

static void
gjs_debug_hooks_dispose(GObject *object)
{
    GjsDebugHooks        *hooks = GJS_DEBUG_HOOKS(object);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    G_OBJECT_CLASS(gjs_debug_hooks_parent_class)->dispose(object);
}

static void
gjs_debug_hooks_finalize(GObject *object)
{
    GjsDebugHooks        *hooks = GJS_DEBUG_HOOKS(object);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    /* Unref scripts_loaded here as there's no guaruntee it will be empty
     * since the garbage-collect phase might happen after we're unreffed */
    g_hash_table_unref(priv->scripts_loaded);

    GHashTable *hashtables_to_unref[] = {
        priv->breakpoints_connections,
        priv->new_script_connections,
        priv->single_step_connections,
        priv->call_and_execute_connections,
        priv->breakpoints,
        priv->pending_breakpoints,
        NULL
    };

    GArray *arrays_to_destroy[] = {
        priv->new_script_hooks,
        priv->call_and_execute_hooks,
        priv->single_step_hooks,
        priv->pc_stack,
        NULL
    };

    unref_all_hashtables(hashtables_to_unref);
    destroy_all_arrays(arrays_to_destroy);

    /* If we've still got usage counts on the context debug hooks then that's
     * an error and we should assert here */
    g_assert(priv->call_and_execute_hook_usage_count == 0);
    g_assert(priv->debug_mode_usage_count == 0);
    g_assert(priv->interrupt_function_usage_count == 0);
    g_assert(priv->new_script_hook_usage_count == 0);
    g_assert(priv->single_step_mode_usage_count == 0);
    
    G_OBJECT_CLASS(gjs_debug_hooks_parent_class)->finalize(object);
}

static void
gjs_debug_hooks_set_property(GObject      *object,
                             unsigned int  prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    GjsDebugHooks                   *hooks = GJS_DEBUG_HOOKS(object);
    GjsDebugHooksPrivate *priv = (GjsDebugHooksPrivate *) gjs_debug_hooks_get_instance_private(hooks);

    switch (prop_id) {
    case PROP_CONTEXT:
        priv->context = GJS_CONTEXT(g_value_get_object(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gjs_debug_hooks_class_init(GjsDebugHooksClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gjs_debug_hooks_set_property;
    object_class->dispose = gjs_debug_hooks_dispose;
    object_class->finalize = gjs_debug_hooks_finalize;

    properties[PROP_0] = NULL;
    properties[PROP_CONTEXT] = g_param_spec_object("context",
                                                   "Context",
                                                   "GjsContext",
                                                   GJS_TYPE_CONTEXT,
                                                   (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

GjsDebugHooks *
gjs_debug_hooks_new(GjsContext *context)
{
    GjsDebugHooks *hooks =
        GJS_DEBUG_HOOKS(g_object_new(GJS_TYPE_DEBUG_HOOKS,
                                                  "context", context,
                                                  NULL));
    return hooks;
}
