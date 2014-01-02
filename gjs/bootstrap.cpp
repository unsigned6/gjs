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

#include <gio/gio.h>

static gboolean
define_bootstrap_environment(JSContext  *context,
                             JSObject  **environment_out)
{
    JSObject *environment = JS_NewObject(context, NULL, NULL, NULL);

    if (!environment)
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
