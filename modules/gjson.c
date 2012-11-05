/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/* vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <jsapi.h>
#include <gio/gio.h>
#include <gjs/gjs-module.h>
#include <gi/object.h>
#include <gjs/compat.h>

#include "gjson.h"

JSBool
gjs_gjson_load(JSContext *context,
               uintN      argc,
               jsval     *vp)
{
    JSBool ret = JS_FALSE;
    GError *local_error = NULL;
    GError **error = &local_error;
    jsval *argv = JS_ARGV(cx, vp);
    JSObject *stream_obj = NULL;
    GInputStream *stream = NULL;
    guchar buf[4096];
    gssize bytes_read;
    JSONParser *parser = NULL;
    jsval rv = JSVAL_NULL;

    if (!gjs_parse_args(context, "gjson_load", "o", argc, argv,
                        "stream", &stream_obj))
        goto out;

    if (!gjs_typecheck_object (context, stream_obj, G_TYPE_INPUT_STREAM, TRUE))
        goto out;
    stream = (GInputStream*)gjs_g_object_from_object (context, stream_obj);

    parser = JS_BeginJSONParse(context, &rv);

    while ((bytes_read = g_input_stream_read (stream, buf, sizeof (buf),
                                              NULL, error)) > 0) {
        if (!JS_ConsumeJSONText(context, parser, (jschar*)buf, (uint32)bytes_read))
            goto out;
    }
    if (bytes_read < 0)
        goto out;

    /* Swap parser to NULL so we don't clean up */
    {
        JSONParser *tmp = parser;
        parser = NULL;
        if (!JS_FinishJSONParse(context, tmp, JSVAL_NULL))
            goto out;
    }

    ret = JS_TRUE;
 out:
    /* Clean up parser if necessary */
    if (parser)
        JS_FinishJSONParse(context, parser, JSVAL_NULL);
    return ret;
}

JSBool
gjs_define_gjson_stuff(JSContext *context,
                       JSObject  *module_obj)
{
    if (!JS_DefineFunction(context, module_obj,
                           "load",
                           (JSNative) gjs_gjson_load,
                           1, GJS_MODULE_PROP_FLAGS))
        return JS_FALSE;

    return JS_TRUE;
}

GJS_REGISTER_NATIVE_MODULE("gjson", gjs_define_gjson_stuff);
