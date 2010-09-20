/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/* Copyright 2010 Red Hat, Inc. All Rights Reserved.
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

#include <gjs/gjs.h>
#include <jsapi.h>
#include <gjs/compat.h>

#include "system.h"

static JSBool
gjs_address_of(JSContext *context,
               JSObject  *obj,
               uintN      argc,
               jsval     *argv,
               jsval     *retval)
{
    JSObject *target_obj;
    if (!gjs_parse_args (context, "addressof", "o", argc, argv, "object", &target_obj))
        return JS_FALSE;
    if (!JS_NewNumberValue(context, (unsigned long) target_obj, retval))
        return JS_FALSE;
    return JS_TRUE;
}

static JSBool
gjs_breakpoint(JSContext *context,
               JSObject  *obj,
               uintN      argc,
               jsval     *argv,
               jsval     *retval)
{
    if (!gjs_parse_args (context, "breakpoint", "", argc, argv))
        return JS_FALSE;
    G_BREAKPOINT();
    return JS_TRUE;
}

JSBool
gjs_js_define_system_stuff(JSContext *context,
                           JSObject  *module)
{
    if (!JS_DefineFunction(context, module,
                           "addressOf",
                           gjs_address_of,
                           1, GJS_MODULE_PROP_FLAGS))
        return JS_FALSE;

    if (!JS_DefineFunction(context, module,
                           "breakpoint",
                           gjs_breakpoint,
                           0, GJS_MODULE_PROP_FLAGS))
        return JS_FALSE;

    return JS_TRUE;
}

GJS_REGISTER_NATIVE_MODULE("system", gjs_js_define_system_stuff)
