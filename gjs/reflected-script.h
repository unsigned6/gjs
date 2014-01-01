/*
 * Copyright © 20134Endless Mobile, Inc.
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
#ifndef GJS_REFLECTED_SCRIPT_H
#define GJS_REFLECTED_SCRIPT_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_TYPE_REFLECTED_SCRIPT gjs_reflected_script_get_type()

#define GJS_REFLECTED_SCRIPT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), \
     GJS_TYPE_REFLECTED_SCRIPT, GjsReflectedScript))

#define GJS_REFLECTED_SCRIPT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), \
     GJS_TYPE_REFLECTED_SCRIPT, GjsReflectedScriptClass))

#define GJS_IS_REFLECTED_SCRIPT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
     GJS_TYPE_REFLECTED_SCRIPT))

#define GJS_IS_REFLECTED_SCRIPT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), \
     GJS_TYPE_REFLECTED_SCRIPT))

#define GJS_REFLECTED_SCRIPT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), \
     GJS_TYPE_REFLECTED_SCRIPT, GjsReflectedScriptClass))

typedef struct _GjsReflectedScript GjsReflectedScript;
typedef struct _GjsReflectedScriptClass GjsReflectedScriptClass;
typedef struct _GjsReflectedScriptPrivate GjsReflectedScriptPrivate;

typedef struct _GjsReflectedScriptBranchInfo GjsReflectedScriptBranchInfo;
typedef struct _GjsReflectedScriptFunctionInfo GjsReflectedScriptFunctionInfo;

typedef struct _GjsContext GjsContext;

struct _GjsReflectedScriptClass {
    GObjectClass parent_class;
};

struct _GjsReflectedScript {
    GObject parent;
};

unsigned int gjs_reflected_script_branch_info_get_branch_point(const GjsReflectedScriptBranchInfo *info);
const unsigned int * gjs_reflected_script_branch_info_get_branch_alternatives(const GjsReflectedScriptBranchInfo *info,
                                                                              unsigned int                       *n);

unsigned int gjs_reflected_script_function_info_get_line_number(const GjsReflectedScriptFunctionInfo *info);
unsigned int gjs_reflected_script_function_info_get_n_params(const GjsReflectedScriptFunctionInfo *info);
const char * gjs_reflected_script_function_info_get_name(const GjsReflectedScriptFunctionInfo *info);

const GjsReflectedScriptFunctionInfo ** gjs_reflected_script_get_functions(GjsReflectedScript *script);
const unsigned int * gjs_reflected_script_get_expression_lines(GjsReflectedScript *script,
                                                               unsigned int       *n_executable_lines);
const GjsReflectedScriptBranchInfo ** gjs_reflected_script_get_branches(GjsReflectedScript *script);
unsigned int gjs_reflected_script_get_n_lines(GjsReflectedScript *script);

GType gjs_reflected_script_get_type(void);

GjsReflectedScript * gjs_reflected_script_new(const char *filename,
                                              GjsContext *reflection_context);

/* Creates a "reflection context" that can be passed to the constructor of
 * gjs_reflected_script_new. This context will have the script
 * containing the functions which permit reflection pre-defined and can
 * be shared across all reflections */
GjsContext * gjs_reflected_script_create_reflection_context();

G_END_DECLS

#endif
