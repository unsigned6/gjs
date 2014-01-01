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

#include <gio/gio.h>

#include <gjs/gjs.h>
#include <gjs/jsapi-util.h>
#include <gjs/compat.h>
#include <gjs/gjs-module.h>

#include <gjs/reflected-script.h>
#include "reflected-script-private.h"

struct _GjsReflectedScriptPrivate {
    /* External context where the reflection happens. We take
     * a reference to it so that we can still do evaluation even
     * if the original owner of the context goes away
     *
     * One important precondition is that the context needs to
     * be "initialized" to a state that we expect it to be in,
     * eg, we need to have loaded the infoReflect.js script
     * and set up some global variables. If it hasn't then
     * this will trigger an assertion */
    GjsContext *reflection_context;

    char *script_filename;

    /* Array of strings, null-terminated */
    GArray *all_function_names;

    /* Array of GjsReflectedScriptBranchInfo */
    GArray *all_branches;

    /* Sorted array of unsigned int */
    GArray *all_expression_lines;

    /* Number of lines */
    unsigned int n_lines;

    /* A flag which indicates whether or not reflection
     * data has been gathered for this script yet. Reflect.parse
     * can be a super-expensive operation for large scripts so
     * we should perform it on-demand when we actually need to */
    gboolean reflection_performed;
};

G_DEFINE_TYPE_WITH_PRIVATE(GjsReflectedScript,
                           gjs_reflected_script,
                           G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_SCRIPT_FILENAME,
    PROP_REFLECTION_CONTEXT,
    PROP_N
};

static GParamSpec *properties[PROP_N];

static void ensure_script_reflected(GjsReflectedScript *script);

GjsReflectedScriptBranchInfo *
gjs_reflected_script_branch_info_new(unsigned int  branch_point,
                                     GArray       *alternatives)
{
    GjsReflectedScriptBranchInfo *info = g_new0(GjsReflectedScriptBranchInfo, 1);
    info->branch_point = branch_point;
    info->branch_alternatives = alternatives;
    return info;
}

void
gjs_reflected_script_branch_info_destroy(gpointer info_data)
{
    GjsReflectedScriptBranchInfo *info = (GjsReflectedScriptBranchInfo *) info_data;
    g_array_free(info->branch_alternatives, TRUE);
    g_free(info);
}

unsigned int
gjs_reflected_script_branch_info_get_branch_point(const GjsReflectedScriptBranchInfo *info)
{
    return info->branch_point;
}

const unsigned int *
gjs_reflected_script_branch_info_get_branch_alternatives(const GjsReflectedScriptBranchInfo *info,
                                                         unsigned int                       *n)
{
    g_return_val_if_fail(n, NULL);

    *n = info->branch_alternatives->len;
    return (unsigned int *) info->branch_alternatives->data;
}

GjsReflectedScriptFunctionInfo *
gjs_reflected_script_function_info_new(char         *name,
                                       unsigned int  line_number,
                                       unsigned int  n_params)
{
    GjsReflectedScriptFunctionInfo *info = (GjsReflectedScriptFunctionInfo *) g_new0(GjsReflectedScriptFunctionInfo, 1);
    info->name = name;
    info->line_number = line_number;
    info->n_params = n_params;

    return info;
}

unsigned int
gjs_reflected_script_function_info_get_line_number(const GjsReflectedScriptFunctionInfo *info)
{
    return info->line_number;
}

unsigned int
gjs_reflected_script_function_info_get_n_params(const GjsReflectedScriptFunctionInfo *info)
{
    return info->n_params;
}

const char *
gjs_reflected_script_function_info_get_name(const GjsReflectedScriptFunctionInfo *info)
{
    return info->name;
}

void
gjs_reflected_script_function_info_destroy(gpointer info_data)
{
    GjsReflectedScriptFunctionInfo *info = (GjsReflectedScriptFunctionInfo *) info_data;

    if (info->name)
        g_free(info->name);

    g_free(info);
}


const GjsReflectedScriptBranchInfo **
gjs_reflected_script_get_branches(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, NULL);

    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    ensure_script_reflected(script);

    g_assert(priv->all_branches);
    return (const GjsReflectedScriptBranchInfo **) priv->all_branches->data;\
}

const GjsReflectedScriptFunctionInfo **
gjs_reflected_script_get_functions(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, NULL);

    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    ensure_script_reflected(script);

    g_assert(priv->all_function_names);
    return (const GjsReflectedScriptFunctionInfo **) priv->all_function_names->data;
}

const unsigned int *
gjs_reflected_script_get_expression_lines(GjsReflectedScript *script,
                                          unsigned int       *n)
{
    g_return_val_if_fail(script, NULL);
    g_return_val_if_fail(n, NULL);

    g_return_val_if_fail(n, NULL);

    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    ensure_script_reflected(script);

    g_assert(priv->all_expression_lines);
    if (priv->all_expression_lines) {
        *n = priv->all_expression_lines->len;
        return (const unsigned int *) priv->all_expression_lines->data;
    }
}

typedef gboolean (*ConvertAndInsertJSVal) (GArray    *array,
                                           JSContext *context,
                                           jsval     *element);

static gboolean
get_array_from_js_value(JSContext             *context,
                        jsval                 *value,
                        size_t                 array_element_size,
                        GDestroyNotify         element_clear_func,
                        ConvertAndInsertJSVal  inserter,
                        GArray                **out_array)
{
    g_return_val_if_fail(out_array != NULL, FALSE);
    g_return_val_if_fail(*out_array == NULL, FALSE);

    JSObject *js_array = JSVAL_TO_OBJECT(*value);

    if (!JS_IsArrayObject(context, js_array)) {
        g_critical("Returned object from is not an array");
        return FALSE;
    }

    /* We're not preallocating any space here at the moment until
     * we have some profiling data that suggests a good size to
     * preallocate to.
     *
     * TODO: Preallocating would be nice. */
    GArray *script_functions_array = g_array_new(TRUE, TRUE, array_element_size);
    u_int32_t script_funtions_array_len;

    if (element_clear_func)
        g_array_set_clear_func(script_functions_array, element_clear_func);

    if (JS_GetArrayLength(context, js_array, &script_funtions_array_len)) {
        u_int32_t i = 0;
        for (; i < script_funtions_array_len; ++i) {
            jsval element;
            if (!JS_GetElement(context, js_array, i, &element)) {
                g_array_unref(script_functions_array);
                gjs_throw(context, "Failed to get function names array element %i", i);
                return FALSE;
            }

            if (!(inserter(script_functions_array, context, &element))) {
                g_array_unref(script_functions_array);
                gjs_throw(context, "Failed to convert array element %i", i);
                return FALSE;
            }
        }
    }

    *out_array = script_functions_array;

    return TRUE;
}

static GArray *
call_js_function_for_array_return(JSContext             *context,
                                  JSObject              *object,
                                  size_t                 array_element_size,
                                  GDestroyNotify         element_clear_func,
                                  ConvertAndInsertJSVal  inserter,
                                  const char            *function_name,
                                  jsval                 *ast)
{
    GArray *array = NULL;
    jsval rval;
    if (!JS_CallFunctionName(context, object, function_name, 1, ast, &rval)) {
        gjs_log_exception(context);
        return NULL;
    }

    if (!get_array_from_js_value(context,
                                 &rval,
                                 array_element_size,
                                 element_clear_func,
                                 inserter,
                                 &array)) {
        gjs_log_exception(context);
        return NULL;
    }

    return array;
}

static void
clear_reflected_script_function_info(gpointer info_location)
{
    GjsReflectedScriptFunctionInfo **info_ptr = (GjsReflectedScriptFunctionInfo **) info_location;

    gjs_reflected_script_function_info_destroy(*info_ptr);
}

static gboolean
convert_and_insert_function_decl(GArray    *array,
                                 JSContext *context,
                                 jsval     *element)
{
    JSObject *object = JSVAL_TO_OBJECT(*element);

    if (!object) {
        gjs_throw(context, "Converting element to object failed");
        return FALSE;
    }

    jsval line_number_property_value;
    if (!JS_GetProperty(context, object, "line", &line_number_property_value) ||
        !JSVAL_IS_INT(line_number_property_value)) {
        gjs_throw(context, "Failed to get line property for function object");
        return FALSE;
    }

    unsigned int line_number = JSVAL_TO_INT(line_number_property_value);

    jsval n_params_property_value;
    if (!JS_GetProperty(context, object, "n_params", &n_params_property_value) ||
        !JSVAL_IS_INT(n_params_property_value)) {
        gjs_throw(context, "Failed to get n_params property for function object");
        return FALSE;
    }

    unsigned int n_params = JSVAL_TO_INT(n_params_property_value);

    jsval    function_name_property_value;

    if (!JS_GetProperty(context, object, "name", &function_name_property_value)) {
        gjs_throw(context, "Failed to get name property for function object");
        return FALSE;
    }

    char *utf8_string;

    if (JSVAL_IS_STRING(function_name_property_value)) {
        if (!gjs_string_to_utf8(context,
                                function_name_property_value,
                                &utf8_string)) {
            gjs_throw(context, "Failed to convert function_name to string");
            return FALSE;
        }
    } else if (JSVAL_IS_NULL(function_name_property_value)) {
        utf8_string = NULL;
    } else {
        gjs_throw(context, "Unexpected type for function_name");
        return FALSE;
    }


    GjsReflectedScriptFunctionInfo *info =
        gjs_reflected_script_function_info_new(utf8_string,
                                               line_number,
                                               n_params);

    g_array_append_val(array, info);

    return TRUE;
}

static GArray *
get_script_functions_from_reflection(JSContext *context,
                                     JSObject  *global,
                                     jsval     *ast)
{
    return call_js_function_for_array_return(context,
                                             global,
                                             sizeof(GjsReflectedScriptFunctionInfo *),
                                             clear_reflected_script_function_info,
                                             convert_and_insert_function_decl,
                                             "functionsForAST",
                                             ast);
}

static gboolean
convert_and_insert_unsigned_int(GArray    *array,
                                JSContext *context,
                                jsval     *element)
{
    if (!JSVAL_IS_INT(*element)) {
        g_critical("Array element is not an integer");
        return FALSE;
    }

    unsigned int element_integer = JSVAL_TO_INT(*element);
    g_array_append_val(array, element_integer);
    return TRUE;
}

static int
uint_compare(gconstpointer left,
             gconstpointer right)
{
    unsigned int *left_int = (unsigned int *) left;
    unsigned int *right_int = (unsigned int *) right;

    return *left_int - *right_int;
}

static GArray *
get_all_lines_with_executable_expressions_from_reflection(JSContext *context,
                                                          JSObject  *global,
                                                          jsval     *ast)
{
    GArray *all_expressions = call_js_function_for_array_return(context,
                                                                global,
                                                                sizeof(unsigned int),
                                                                NULL,
                                                                convert_and_insert_unsigned_int,
                                                                "expressionLinesForAST",
                                                                ast);

    /* Sort, just to be sure */
    g_array_sort(all_expressions, uint_compare);
    return all_expressions;
}

static void
gjs_reflected_script_branch_info_clear(gpointer branch_info_location)
{
    GjsReflectedScriptBranchInfo **info_ptr = (GjsReflectedScriptBranchInfo **) branch_info_location;
    gjs_reflected_script_branch_info_destroy(*info_ptr);
}

static gboolean
convert_and_insert_branch_info(GArray    *array,
                               JSContext *context,
                               jsval     *element)
{
    if (!JSVAL_IS_OBJECT(*element)) {
        gjs_throw(context, "Array element is not an object");
        return FALSE;
    }

    JSObject *object = JSVAL_TO_OBJECT(*element);

    if (!object) {
        gjs_throw(context, "Converting element to object failed");
        return FALSE;
    }

    jsval   branch_point_value;
    int32_t branch_point;

    if (!JS_GetProperty(context, object, "point", &branch_point_value) ||
        !JSVAL_IS_INT(branch_point_value)) {
        gjs_throw(context, "Failed to get point property from element");
        return FALSE;
    }

    branch_point = JSVAL_TO_INT(branch_point_value);

    jsval  branch_exits_value;
    GArray *branch_exists_array = NULL;

    if (!JS_GetProperty(context, object, "exits", &branch_exits_value) ||
        !JSVAL_IS_OBJECT(branch_exits_value)) {
        gjs_throw(context, "Failed to get exits property from element");
        return FALSE;
    }

    if (!get_array_from_js_value(context,
                                 &branch_exits_value,
                                 sizeof(unsigned int),
                                 NULL,
                                 convert_and_insert_unsigned_int,
                                 &branch_exists_array)) {
        /* Already logged the exception, no need to do anything here */
        return FALSE;
    }

    GjsReflectedScriptBranchInfo *info = gjs_reflected_script_branch_info_new(branch_point,
                                                                              branch_exists_array);

    g_array_append_val(array, info);

    return TRUE;
}

static GArray *
get_script_branches_from_reflection(JSContext *context,
                                    JSObject  *global,
                                    jsval     *ast)
{
    return call_js_function_for_array_return(context,
                                             global,
                                             sizeof(GjsReflectedScriptBranchInfo *),
                                             gjs_reflected_script_branch_info_clear,
                                             convert_and_insert_branch_info,
                                             "branchesForAST",
                                             ast);
}

static unsigned int
count_lines_in_script(const char *data)
{
    int lines = 1;
    for (; *data; ++data)
        if (*data == '\n')
            ++lines;
    return lines;
}

static JSString *
load_script_for_reflection(GjsContext   *context,
                           const char   *filename,
                           int          *start_line_number,
                           unsigned int *script_n_lines)
{
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(context);
    GFile     *script_file = g_file_new_for_commandline_arg(filename);

    if (!g_file_query_exists(script_file, NULL)) {
        g_object_unref(script_file);
        gjs_throw(js_context, "Script: %s does not exist!", filename);
        return NULL;
    }

    char  *original_script_contents;
    gsize script_contents_len;
    if (!g_file_load_contents(script_file,
                              NULL,
                              &original_script_contents,
                              &script_contents_len,
                              NULL,
                              NULL)) {
        g_object_unref(script_file);
        gjs_throw(js_context, "Failed to get script contents for %s", filename);
        return FALSE;
    }

    /* Number of lines in the script must be based on the original script contents
     * as we get line numbers relative to the starting line there */
    *script_n_lines = count_lines_in_script(original_script_contents);

    /* gjs_script_unix_shebang will modify start_line_number if necessary */
    const char *modified_script_contents =
        gjs_strip_unix_shebang(original_script_contents,
                               (gssize *) &script_contents_len,
                               start_line_number);

    JSString *str = JS_NewStringCopyZ(js_context, modified_script_contents);

    g_free(original_script_contents);
    g_object_unref(script_file);

    return str;
}

static gboolean
perform_reflection_within_compartment(GjsContext         *internal_context,
                                      GjsReflectedScript *script)
{
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(internal_context);
    JSObject *global = JS_GetGlobalObject(js_context);

    JSAutoCompartment ac(js_context, global);

    int          start_line_number = 1;
    unsigned int script_n_lines;

    JSString *str = load_script_for_reflection(internal_context,
                                               priv->script_filename,
                                               &start_line_number,
                                               &script_n_lines);

    if (!str)
        return FALSE;

    jsval reflect_object_value;
    if (!JS_GetProperty(js_context, global, "Reflect", &reflect_object_value) ||
        !JSVAL_IS_OBJECT(reflect_object_value)) {
        gjs_throw(js_context, "'Reflect' object not found in context");
        return FALSE;
    }

    JSObject *reflect_object = JSVAL_TO_OBJECT(reflect_object_value);
    JSObject *reflect_options_object = JS_NewObject(js_context, NULL, NULL, NULL);

    jsval loc_value = BOOLEAN_TO_JSVAL(JS_TRUE);
    jsval line_value = INT_TO_JSVAL(start_line_number);

    JS_SetProperty(js_context, reflect_options_object, "loc", &loc_value);
    JS_SetProperty(js_context, reflect_options_object, "line", &line_value);

    jsval parseArgv[] = {
        STRING_TO_JSVAL(str),
        OBJECT_TO_JSVAL(reflect_options_object)
    };
    jsval ast_value;

    if (!JS_CallFunctionName(js_context, reflect_object, "parse", 2, parseArgv, &ast_value)) {
        gjs_throw(js_context, "Failed to call Reflect.parse");
        return FALSE;
    }

    priv->all_function_names = get_script_functions_from_reflection(js_context,
                                                                    global,
                                                                    &ast_value);
    priv->all_branches = get_script_branches_from_reflection(js_context,
                                                             global,
                                                             &ast_value);
    priv->all_expression_lines = get_all_lines_with_executable_expressions_from_reflection(js_context,
                                                                                           global,
                                                                                           &ast_value);
    priv->n_lines = script_n_lines;

    return TRUE;
}

static void
ensure_script_reflected(GjsReflectedScript *script)
{
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    if (priv->reflection_performed)
        return;

    if (!perform_reflection_within_compartment(priv->reflection_context, script)) {
        g_warning("Reflecting script %s failed", priv->script_filename);
        /* If the reflection failed, we should make sure that the the reflection
         * details have sane defaults */
        priv->all_branches = g_array_new(TRUE, TRUE, sizeof(GjsReflectedScriptBranchInfo));
        priv->all_function_names = g_array_new(TRUE, TRUE, sizeof(GjsReflectedScriptFunctionInfo));
        priv->all_expression_lines = g_array_new(TRUE, TRUE, sizeof(unsigned int));
        priv->n_lines = 0;
    }

    priv->reflection_performed = TRUE;
}

GjsContext *
gjs_reflected_script_create_reflection_context()
{
    static const char *resource_path = "resource:///org/gnome/gjs/modules/infoReflect.js";

    GjsContext *context = gjs_context_new();

    if (!gjs_context_eval_file(context,
                               resource_path,
                               NULL,
                               NULL)) {
        g_object_unref(context);
        return NULL;
    }

    gjs_context_pop();

    return context;
}

unsigned int
gjs_reflected_script_get_n_lines(GjsReflectedScript *script)
{
    g_return_val_if_fail(script, 0);

    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    ensure_script_reflected(script);

    return priv->n_lines;
}

static void
gjs_reflected_script_init(GjsReflectedScript *script)
{
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    priv->all_branches = NULL;
    priv->all_function_names = NULL;
    priv->all_expression_lines = NULL;
}

static void
unref_array_if_nonnull(GArray *array)
{
    if (array)
        g_array_unref(array);
}

static void
gjs_reflected_script_dispose(GObject *object)
{
    GjsReflectedScript *script = GJS_REFLECTED_SCRIPT(object);
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    g_clear_object(&priv->reflection_context);

    G_OBJECT_CLASS(gjs_reflected_script_parent_class)->dispose(object);
}

static void
gjs_reflected_script_finalize(GObject *object)
{
    GjsReflectedScript *script = GJS_REFLECTED_SCRIPT(object);
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    unref_array_if_nonnull(priv->all_branches);
    unref_array_if_nonnull(priv->all_function_names);
    unref_array_if_nonnull(priv->all_expression_lines);

    g_free(priv->script_filename);

    G_OBJECT_CLASS(gjs_reflected_script_parent_class)->finalize(object);
}

static void
gjs_reflected_script_set_property(GObject      *object,
                                  unsigned int  prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    GjsReflectedScript *script = GJS_REFLECTED_SCRIPT(object);
    GjsReflectedScriptPrivate *priv = (GjsReflectedScriptPrivate *) gjs_reflected_script_get_instance_private(script);

    switch (prop_id) {
    case PROP_SCRIPT_FILENAME:
        priv->script_filename = g_value_dup_string(value);
        break;
    case PROP_REFLECTION_CONTEXT:
        priv->reflection_context = (GjsContext *) g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gjs_reflected_script_class_init(GjsReflectedScriptClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gjs_reflected_script_set_property;
    object_class->dispose = gjs_reflected_script_dispose;
    object_class->finalize = gjs_reflected_script_finalize;

    properties[PROP_0] = NULL;
    properties[PROP_SCRIPT_FILENAME] = g_param_spec_string("filename",
                                                           "Script Filename",
                                                           "Valid path to script",
                                                           NULL,
                                                           (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    properties[PROP_REFLECTION_CONTEXT] = g_param_spec_object("reflection-context",
                                                              "Reflection Context",
                                                              "Context to perform reflection in. "
                                                              "This must have been initialized with infoReflect.js loaded ",
                                                              GJS_TYPE_CONTEXT,
                                                              (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

GjsReflectedScript *
gjs_reflected_script_new(const char *filename,
                         GjsContext *reflection_context)
{
    GjsReflectedScript *script =
        GJS_REFLECTED_SCRIPT(g_object_new(GJS_TYPE_REFLECTED_SCRIPT,
                                          "filename", filename,
                                          "reflection-context", reflection_context,
                                          NULL));
    return script;
}
