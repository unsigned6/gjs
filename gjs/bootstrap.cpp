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

#include "config.h"

#include <gjs/gjs.h>

#include "bootstrap.h"
#include "native.h"

#include <gio/gio.h>

/* The bootstrap process is the thing that sets up the import system.
 * As such, we give it a hook to import any native modules it may need.
 *
 * The rest of the functionality that the bootstrap code needs should be
 * in independent native modules which can be imported by this API,
 * rather than in the bootstrap environment.
 */

static JSBool
import_native_module(JSContext *context,
                     unsigned   argc,
                     jsval     *vp)
{
    JSBool ret = JS_FALSE;
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    char *module_name = NULL;
    JSObject *module_obj;

    if (!gjs_parse_call_args(context, "importNativeModule", "s", args,
                             "moduleName", &module_name))
        goto out;

    if (!gjs_import_native_module(context, module_name, &module_obj))
        goto out;

    ret = JS_TRUE;
    args.rval().setObjectOrNull(module_obj);

 out:
    g_free(module_name);
    return ret;
}

static JSFunctionSpec environment_funcs[] = {
    { "importNativeModule", JSOP_WRAPPER (import_native_module), 1, GJS_MODULE_PROP_FLAGS },
    { NULL },
};

static gboolean
define_bootstrap_environment(JSContext  *context,
                             JSObject  **environment_out)
{
    JSObject *environment = JS_NewObject(context, NULL, NULL, NULL);

    if (!environment)
        return FALSE;

    if (!JS_DefineFunctions(context, environment, &environment_funcs[0]))
        return FALSE;

    *environment_out = environment;
    return TRUE;
}

#define BOOTSTRAP_FILE "resource:///org/gnome/gjs/modules/bootstrap.js"

gboolean
gjs_run_bootstrap(JSContext *context)
{
    GFile *file = g_file_new_for_uri(BOOTSTRAP_FILE);
    JSObject *environment;
    char *script = NULL;
    gsize script_len = 0;
    jsval script_retval;

    if (!define_bootstrap_environment(context, &environment))
        return FALSE;

    if (!g_file_load_contents(file, NULL, &script, &script_len, NULL, NULL))
        return FALSE;

    if (!gjs_eval_with_scope(context, environment, script, script_len, BOOTSTRAP_FILE, NULL))
        return FALSE;

    return TRUE;
}
