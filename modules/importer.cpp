/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright 2013 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include "importer.h"
#include <gjs/gjs-module.h>
#include <gjs/byteArray.h>
#include "gi/ns.h"

static JSBool
import_gi_module(JSContext *context,
                 unsigned   argc,
                 jsval     *vp)
{
    JSBool ret = JS_FALSE;
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    jsval retval = JSVAL_VOID;
    char *module_name = NULL;
    char *module_version = NULL;
    JSObject *module_obj;

    if (!gjs_parse_call_args(context, "importGIModule", "s?s", args,
                             "moduleName", &module_name,
                             "moduleVersion", &module_version))
        goto out;

    if (!gjs_import_gi_module(context, module_name, module_version, &module_obj))
        goto out;

    ret = JS_TRUE;
    args.rval().setObject(*module_obj);

 out:
    g_free(module_name);
    g_free(module_version);
    return ret;
}

static JSBool
eval_with_scope(JSContext *context,
                unsigned   argc,
                jsval     *vp)
{
    JSBool ret = JS_FALSE;
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JSObject *scope;
    JSObject *script_obj;
    guint8 *script;
    gsize script_len;
    char *filename = NULL;
    jsval retval;

    if (!gjs_parse_call_args(context, "evalWithScope", "oos", args,
                             "scope", &scope,
                             "script", &script_obj,
                             "filename", &filename))
        goto out;

    gjs_byte_array_peek_data (context, script_obj, &script, &script_len);

    if (!gjs_eval_with_scope(context, scope, (const char *) script, script_len, filename, &retval))
        goto out;

    ret = JS_TRUE;
    args.rval().set(retval);

 out:
    g_free(filename);
    return ret;
}

static JSBool
get_builtin_search_path(JSContext *context,
                        unsigned   argc,
                        jsval     *vp)
{
    GjsContext *gjs_context = GJS_CONTEXT(JS_GetContextPrivate(context));
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    const char **context_search_path;
    int context_search_path_length;
    const char **global_search_path;
    int global_search_path_length;
    GArray *elems;
    JSObject *search_path_obj;
    int i;

    context_search_path = gjs_context_get_search_path(gjs_context);
    context_search_path_length = context_search_path ? g_strv_length((char **) context_search_path) : 0;
    global_search_path = (const char **) gjs_get_search_path();
    global_search_path_length = global_search_path ? g_strv_length((char **) global_search_path) : 0;

    elems = g_array_sized_new(FALSE, FALSE, sizeof(jsval),
                              context_search_path_length + global_search_path_length);

    for (i = 0; i < context_search_path_length; i++) {
        jsval element = STRING_TO_JSVAL(JS_NewStringCopyZ(context, context_search_path[i]));
        g_array_append_val(elems, element);
    }

    for (i = 0; i < global_search_path_length; i++) {
        jsval element = STRING_TO_JSVAL(JS_NewStringCopyZ(context, global_search_path[i]));
        g_array_append_val(elems, element);
    }

    search_path_obj = JS_NewArrayObject(context, elems->len, (jsval *)elems->data);
    g_array_free(elems, TRUE);

    args.rval().setObject(*search_path_obj);
    return JS_TRUE;
}

static JSFunctionSpec module_funcs[] = {
    { "importGIModule", JSOP_WRAPPER (import_gi_module), 2, GJS_MODULE_PROP_FLAGS },
    { "evalWithScope", JSOP_WRAPPER (eval_with_scope), 3, GJS_MODULE_PROP_FLAGS },
    { "getBuiltinSearchPath", JSOP_WRAPPER (get_builtin_search_path), 0, GJS_MODULE_PROP_FLAGS },
    { NULL },
};

JSBool
gjs_js_define_importer_stuff(JSContext  *context,
                             JSObject  **module_out)
{
    JSObject *module;

    module = JS_NewObject(context, NULL, NULL, NULL);

    if (!JS_DefineFunctions(context, module, &module_funcs[0]))
        return JS_FALSE;

    *module_out = module;
    return JS_TRUE;
}
