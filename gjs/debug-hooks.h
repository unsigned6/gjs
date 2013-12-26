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
#ifndef GJS_DEBUG_HOOKS_H
#define GJS_DEBUG_HOOKS_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GJS_TYPE_DEBUG_HOOKS gjs_debug_hooks_get_type()

#define GJS_DEBUG_HOOKS(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), \
     GJS_TYPE_DEBUG_HOOKS, GjsDebugHooks))

#define GJS_DEBUG_HOOKS_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), \
     GJS_TYPE_DEBUG_HOOKS, GjsDebugHooksClass))

#define GJS_IS_DEBUG_HOOKS(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
     GJS_TYPE_DEBUG_HOOKS))

#define GJS_IS_DEBUG_HOOKS_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), \
     GJS_TYPE_DEBUG_HOOKS))

#define GJS_DEBUG_HOOKS_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), \
     GJS_TYPE_DEBUG_HOOKS, GjsDebugHooksClass))

typedef struct _GjsContext GjsContext;
typedef struct _GjsReflectedScript GjsReflectedScript;
typedef struct _GjsDebugScriptInfo GjsDebugScriptInfo;
typedef struct _GjsLocationInfo GjsLocationInfo;
typedef struct _GjsFrameInfo GjsFrameInfo;

/* An enum to describe which stage of frame execution we are in.
 *
 * An interrupt will be delivered twice per each entered frame, once
 * upon entry and once upon exit. This information is useful to
 * some tools, for instance, profilers. BEFORE means before we hit
 * the function and AFTER means just after its last instruction finished */
typedef enum _GjsFrameState {
    GJS_FRAME_ENTRY = 0,
    GJS_FRAME_EXIT = 1
} GjsFrameState;

typedef struct _GjsFunctionKey {
    const char   *filename;
    const char   *function_name;
    unsigned int line;
    unsigned int n_args;
} GjsFunctionKey;

typedef struct _GjsFrameInfo {
    unsigned int   current_line;
    GjsFunctionKey current_function;
} GjsFrameInfo;

/**
 * gjs_location_info_get_current_frame:
 * @info: A #GjsLocationInfo
 *
 * This function returns the current stack frame, including function
 * name and position for a #GjsLocationInfo .
 *
 * Return: (transfer-none): A #GjsFrameInfo for the current stack frame
 */
const GjsFrameInfo * gjs_location_info_get_current_frame(const GjsLocationInfo *info);

const char * gjs_debug_script_info_get_filename(const GjsDebugScriptInfo *info);
unsigned int gjs_debug_script_info_get_begin_line(const GjsDebugScriptInfo *info);

typedef struct _GjsDebugHooks GjsDebugHooks;
typedef struct _GjsDebugHooksClass GjsDebugHooksClass;
typedef struct _GjsDebugHooksPrivate GjsDebugHooksPrivate;

typedef struct _GjsContext GjsContext;

struct _GjsDebugHooksClass {
    GObjectClass parent_class;
};

struct _GjsDebugHooks {
    GObject parent;
};

typedef void (*GjsFrameCallback)(GjsDebugHooks   *hooks,
                                 GjsContext      *context,
                                 GjsLocationInfo *info,
                                 GjsFrameState    state,
                                 gpointer         user_data);

typedef void (*GjsInterruptCallback)(GjsDebugHooks   *hooks,
                                     GjsContext      *context,
                                     GjsLocationInfo *info,
                                     gpointer         user_data);

typedef void (*GjsInfoCallback) (GjsDebugHooks      *hooks,
                                 GjsContext         *context,
                                 GjsDebugScriptInfo *info,
                                 gpointer            user_data);


guint
gjs_debug_hooks_add_breakpoint(GjsDebugHooks        *hooks,
                               const char           *filename,
                               unsigned int          line,
                               GjsInterruptCallback  callback,
                               gpointer              user_data);

void
gjs_debug_hooks_remove_breakpoint(GjsDebugHooks *hooks,
                                  guint          handle);

guint
gjs_debug_hooks_add_singlestep_hook(GjsDebugHooks        *hooks,
                                    GjsInterruptCallback  callback,
                                    gpointer              user_data);

void
gjs_debug_hooks_remove_singlestep_hook(GjsDebugHooks *hooks,
                                       guint          handle);

guint
gjs_debug_hooks_add_script_load_hook(GjsDebugHooks   *hooks,
                                     GjsInfoCallback  callback,
                                     gpointer         user_data);

void
gjs_debug_hooks_remove_script_load_hook(GjsDebugHooks *hooks,
                                        guint          handle);

guint
gjs_debug_hooks_add_frame_step_hook(GjsDebugHooks    *hooks,
                                    GjsFrameCallback  callback,
                                    gpointer          user_data);

void
gjs_debug_hooks_remove_frame_step_hook(GjsDebugHooks *hooks,
                                       guint          handle);

GType gjs_debug_hooks_get_type(void);

GjsDebugHooks * gjs_debug_hooks_new(GjsContext *context);

G_END_DECLS

#endif
