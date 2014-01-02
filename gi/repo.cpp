/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
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

#include "repo.h"
#include "ns.h"
#include "function.h"
#include "object.h"
#include "param.h"
#include "boxed.h"
#include "union.h"
#include "enumeration.h"
#include "arg.h"
#include "foreign.h"
#include "fundamental.h"
#include "interface.h"
#include "gerror.h"

#include <gjs/compat.h>
#include <gjs/jsapi-private.h>
#include <gjs/runtime.h>

#include <util/log.h>
#include <util/misc.h>

#include <girepository.h>
#include <string.h>

static JSBool
gjs_define_constant(JSContext      *context,
                    JSObject       *in_object,
                    GIConstantInfo *info)
{
    jsval value;
    GArgument garg = { 0, };
    GITypeInfo *type_info;
    const char *name;
    JSBool ret = JS_FALSE;

    type_info = g_constant_info_get_type(info);
    g_constant_info_get_value(info, &garg);

    if (!gjs_value_from_g_argument(context, &value, type_info, &garg, TRUE))
        goto out;

    name = g_base_info_get_name((GIBaseInfo*) info);

    if (JS_DefineProperty(context, in_object,
                          name, value,
                          NULL, NULL,
                          GJS_MODULE_PROP_FLAGS))
        ret = JS_TRUE;

 out:
    g_constant_info_free_value (info, &garg);
    g_base_info_unref((GIBaseInfo*) type_info);
    return ret;
}

#if GJS_VERBOSE_ENABLE_GI_USAGE
void
_gjs_log_info_usage(GIBaseInfo *info)
{
#define DIRECTION_STRING(d) ( ((d) == GI_DIRECTION_IN) ? "IN" : ((d) == GI_DIRECTION_OUT) ? "OUT" : "INOUT" )
#define TRANSFER_STRING(t) ( ((t) == GI_TRANSFER_NOTHING) ? "NOTHING" : ((t) == GI_TRANSFER_CONTAINER) ? "CONTAINER" : "EVERYTHING" )

    {
        char *details;
        GIInfoType info_type;
        GIBaseInfo *container;

        info_type = g_base_info_get_type(info);

        if (info_type == GI_INFO_TYPE_FUNCTION) {
            GString *args;
            int n_args;
            int i;
            GITransfer retval_transfer;

            args = g_string_new("{ ");

            n_args = g_callable_info_get_n_args((GICallableInfo*) info);
            for (i = 0; i < n_args; ++i) {
                GIArgInfo *arg;
                GIDirection direction;
                GITransfer transfer;

                arg = g_callable_info_get_arg((GICallableInfo*)info, i);
                direction = g_arg_info_get_direction(arg);
                transfer = g_arg_info_get_ownership_transfer(arg);

                g_string_append_printf(args,
                                       "{ GI_DIRECTION_%s, GI_TRANSFER_%s }, ",
                                       DIRECTION_STRING(direction),
                                       TRANSFER_STRING(transfer));

                g_base_info_unref((GIBaseInfo*) arg);
            }
            if (args->len > 2)
                g_string_truncate(args, args->len - 2); /* chop comma */

            g_string_append(args, " }");

            retval_transfer = g_callable_info_get_caller_owns((GICallableInfo*) info);

            details = g_strdup_printf(".details = { .func = { .retval_transfer = GI_TRANSFER_%s, .n_args = %d, .args = %s } }",
                                      TRANSFER_STRING(retval_transfer), n_args, args->str);
            g_string_free(args, TRUE);
        } else {
            details = g_strdup_printf(".details = { .nothing = {} }");
        }

        container = g_base_info_get_container(info);

        gjs_debug_gi_usage("{ GI_INFO_TYPE_%s, \"%s\", \"%s\", \"%s\", %s },",
                           gjs_info_type_name(info_type),
                           g_base_info_get_namespace(info),
                           container ? g_base_info_get_name(container) : "",
                           g_base_info_get_name(info),
                           details);
        g_free(details);
    }
}
#endif /* GJS_VERBOSE_ENABLE_GI_USAGE */

JSBool
gjs_define_info(JSContext  *context,
                JSObject   *in_object,
                GIBaseInfo *info,
                gboolean   *defined)
{
#if GJS_VERBOSE_ENABLE_GI_USAGE
    _gjs_log_info_usage(info);
#endif

    *defined = TRUE;

    switch (g_base_info_get_type(info)) {
    case GI_INFO_TYPE_FUNCTION:
        {
            JSObject *f;
            f = gjs_define_function(context, in_object, 0, (GICallableInfo*) info);
            if (f == NULL)
                return JS_FALSE;
        }
        break;
    case GI_INFO_TYPE_OBJECT:
        {
            GType gtype;
            gtype = g_registered_type_info_get_g_type((GIRegisteredTypeInfo*)info);

            if (g_type_is_a (gtype, G_TYPE_PARAM)) {
                gjs_define_param_class(context, in_object);
            } else if (g_type_is_a (gtype, G_TYPE_OBJECT)) {
                gjs_define_object_class(context, in_object, (GIObjectInfo*) info, gtype, NULL);
            } else if (G_TYPE_IS_INSTANTIATABLE(gtype)) {
                if (!gjs_define_fundamental_class(context, in_object, (GIObjectInfo*)info, NULL, NULL)) {
                    gjs_throw (context,
                               "Unsupported fundamental class creation for type %s",
                               g_type_name(gtype));
                    return JS_FALSE;
                }
            } else {
                gjs_throw (context,
                           "Unsupported type %s, deriving from fundamental %s",
                           g_type_name(gtype), g_type_name(g_type_fundamental(gtype)));
                return JS_FALSE;
            }
        }
        break;
    case GI_INFO_TYPE_STRUCT:
        /* We don't want GType structures in the namespace,
           we expose their fields as vfuncs and their methods
           as static methods
        */
        if (g_struct_info_is_gtype_struct((GIStructInfo*) info)) {
            *defined = FALSE;
            break;
        }
        /* Fall through */

    case GI_INFO_TYPE_BOXED:
        gjs_define_boxed_class(context, in_object, (GIBoxedInfo*) info);
        break;
    case GI_INFO_TYPE_UNION:
        if (!gjs_define_union_class(context, in_object, (GIUnionInfo*) info))
            return JS_FALSE;
        break;
    case GI_INFO_TYPE_ENUM:
        if (g_enum_info_get_error_domain((GIEnumInfo*) info)) {
            /* define as GError subclass */
            gjs_define_error_class(context, in_object, (GIEnumInfo*) info);
            break;
        }
        /* fall through */

    case GI_INFO_TYPE_FLAGS:
        if (!gjs_define_enumeration(context, in_object, (GIEnumInfo*) info))
            return JS_FALSE;
        break;
    case GI_INFO_TYPE_CONSTANT:
        if (!gjs_define_constant(context, in_object, (GIConstantInfo*) info))
            return JS_FALSE;
        break;
    case GI_INFO_TYPE_INTERFACE:
        gjs_define_interface_class(context, in_object, (GIInterfaceInfo*) info);
        break;
    default:
        gjs_throw(context, "API of type %s not implemented, cannot define %s.%s",
                  gjs_info_type_name(g_base_info_get_type(info)),
                  g_base_info_get_namespace(info),
                  g_base_info_get_name(info));
        return JS_FALSE;
    }

    return JS_TRUE;
}

/* Get the "unknown namespace", which should be used for unnamespaced types */
JSObject*
gjs_lookup_private_namespace(JSContext *context)
{
    jsid ns_name;
    ns_name = gjs_context_get_const_string(context, GJS_STRING_PRIVATE_NS_MARKER);
    return gjs_lookup_namespace_object_by_name(context, ns_name);
}

/* Get the namespace object that the GIBaseInfo should be inside */
JSObject*
gjs_lookup_namespace_object(JSContext  *context,
                            GIBaseInfo *info)
{
    const char *ns;
    jsid ns_name;

    ns = g_base_info_get_namespace(info);
    if (ns == NULL) {
        gjs_throw(context, "%s '%s' does not have a namespace",
                     gjs_info_type_name(g_base_info_get_type(info)),
                     g_base_info_get_name(info));

        return NULL;
    }

    ns_name = gjs_intern_string_to_id(context, ns);
    return gjs_lookup_namespace_object_by_name(context, ns_name);
}

JSObject*
gjs_lookup_namespace_object_by_name(JSContext      *context,
                                    jsid            ns_name)
{
    char *name;
    char *script;
    jsval ns_val;
    JSObject *ns_obj = NULL;

    JS_BeginRequest(context);

    if (!gjs_get_string_id(context, ns_name, &name))
        goto out;

    script = g_strdup_printf("imports.gi.%s;", name);
    if (!gjs_eval_with_scope(context, NULL, script, -1, "<internal>", &ns_val))
        goto out;

    ns_obj = JSVAL_TO_OBJECT(ns_val);

 out:
    JS_EndRequest(context);
    return ns_obj;
}

const char*
gjs_info_type_name(GIInfoType type)
{
    switch (type) {
    case GI_INFO_TYPE_INVALID:
        return "INVALID";
    case GI_INFO_TYPE_FUNCTION:
        return "FUNCTION";
    case GI_INFO_TYPE_CALLBACK:
        return "CALLBACK";
    case GI_INFO_TYPE_STRUCT:
        return "STRUCT";
    case GI_INFO_TYPE_BOXED:
        return "BOXED";
    case GI_INFO_TYPE_ENUM:
        return "ENUM";
    case GI_INFO_TYPE_FLAGS:
        return "FLAGS";
    case GI_INFO_TYPE_OBJECT:
        return "OBJECT";
    case GI_INFO_TYPE_INTERFACE:
        return "INTERFACE";
    case GI_INFO_TYPE_CONSTANT:
        return "CONSTANT";
    case GI_INFO_TYPE_UNION:
        return "UNION";
    case GI_INFO_TYPE_VALUE:
        return "VALUE";
    case GI_INFO_TYPE_SIGNAL:
        return "SIGNAL";
    case GI_INFO_TYPE_VFUNC:
        return "VFUNC";
    case GI_INFO_TYPE_PROPERTY:
        return "PROPERTY";
    case GI_INFO_TYPE_FIELD:
        return "FIELD";
    case GI_INFO_TYPE_ARG:
        return "ARG";
    case GI_INFO_TYPE_TYPE:
        return "TYPE";
    case GI_INFO_TYPE_UNRESOLVED:
        return "UNRESOLVED";
    case GI_INFO_TYPE_INVALID_0:
        g_assert_not_reached();
        break;
    }

    return "???";
}

char*
gjs_camel_from_hyphen(const char *hyphen_name)
{
    GString *s;
    const char *p;
    gboolean next_upper;

    s = g_string_sized_new(strlen(hyphen_name) + 1);

    next_upper = FALSE;
    for (p = hyphen_name; *p; ++p) {
        if (*p == '-' || *p == '_') {
            next_upper = TRUE;
        } else {
            if (next_upper) {
                g_string_append_c(s, g_ascii_toupper(*p));
                next_upper = FALSE;
            } else {
                g_string_append_c(s, *p);
            }
        }
    }

    return g_string_free(s, FALSE);
}

char*
gjs_hyphen_from_camel(const char *camel_name)
{
    GString *s;
    const char *p;

    /* four hyphens should be reasonable guess */
    s = g_string_sized_new(strlen(camel_name) + 4 + 1);

    for (p = camel_name; *p; ++p) {
        if (g_ascii_isupper(*p)) {
            g_string_append_c(s, '-');
            g_string_append_c(s, g_ascii_tolower(*p));
        } else {
            g_string_append_c(s, *p);
        }
    }

    return g_string_free(s, FALSE);
}

JSObject *
gjs_lookup_generic_constructor(JSContext  *context,
                               GIBaseInfo *info)
{
    JSObject *in_object;
    JSObject *constructor;
    const char *constructor_name;
    jsval value;

    in_object = gjs_lookup_namespace_object(context, (GIBaseInfo*) info);
    constructor_name = g_base_info_get_name((GIBaseInfo*) info);

    if (G_UNLIKELY (!in_object))
        return NULL;

    if (!JS_GetProperty(context, in_object, constructor_name, &value))
        return NULL;

    if (G_UNLIKELY (!JSVAL_IS_OBJECT(value) || JSVAL_IS_NULL(value)))
        return NULL;

    constructor = JSVAL_TO_OBJECT(value);
    g_assert(constructor != NULL);

    return constructor;
}

JSObject *
gjs_lookup_generic_prototype(JSContext  *context,
                             GIBaseInfo *info)
{
    JSObject *constructor;
    jsval value;

    constructor = gjs_lookup_generic_constructor(context, info);
    if (G_UNLIKELY (constructor == NULL))
        return NULL;

    if (!gjs_object_get_property_const(context, constructor,
                                       GJS_STRING_PROTOTYPE, &value))
        return NULL;

    if (G_UNLIKELY (!JSVAL_IS_OBJECT(value)))
        return NULL;

    return JSVAL_TO_OBJECT(value);
}
