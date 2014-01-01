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
#ifndef GJS_REFLECTED_SCRIPT_PRIVATE_H
#define GJS_REFLECTED_SCRIPT_PRIVATE_H

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _GjsReflectedScriptBranchInfo {
    unsigned int branch_point;
    GArray       *branch_alternatives;
} GjsReflectedScriptBranchInfo;

GjsReflectedScriptBranchInfo *
gjs_reflected_script_branch_info_new(unsigned int branch_point,
                                     GArray       *alternatives);

void
gjs_reflected_script_branch_info_destroy(gpointer info);

typedef struct _GjsReflectedScriptFunctionInfo {
    unsigned int n_params;
    unsigned int line_number;
    char         *name;
} GjsReflectedScriptFunctionInfo;

/* This function takes ownership of the string */
GjsReflectedScriptFunctionInfo *
gjs_reflected_script_function_info_new(char         *name,
                                       unsigned int line_number,
                                       unsigned int n_params);

void
gjs_reflected_script_function_info_destroy(gpointer info);

G_END_DECLS

#endif
