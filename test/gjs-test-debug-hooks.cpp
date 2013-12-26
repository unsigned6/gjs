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

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

#include <jsapi.h>
#include <jsdbgapi.h>
#include <gjs/gjs.h>
#include <gjs/jsapi-util.h>
#include <gjs/debug-hooks.h>
#include <gjs/reflected-script.h>

typedef struct _GjsDebugHooksFixture {
    GjsContext     *context;
    GjsDebugHooks  *debug_hooks;
    char           *temporary_js_script_filename;
    int             temporary_js_script_open_handle;
} GjsDebugHooksFixture;

static void
gjs_debug_hooks_fixture_set_up (gpointer      fixture_data,
                                gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char           *js_script = "function f () { return 1; }\n";
    fixture->context = gjs_context_new();
    fixture->debug_hooks = gjs_debug_hooks_new(fixture->context);
    fixture->temporary_js_script_open_handle = g_file_open_tmp("mock-js-XXXXXXX.js",
                                                               &fixture->temporary_js_script_filename,
                                                               NULL);

    if (write(fixture->temporary_js_script_open_handle, js_script, strlen(js_script) * sizeof (char)) == -1)
        g_print("Error writing to temporary file: %s", strerror(errno));
}

static void
gjs_debug_hooks_fixture_tear_down(gpointer      fixture_data,
                                  gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    unlink(fixture->temporary_js_script_filename);
    g_free(fixture->temporary_js_script_filename);
    close(fixture->temporary_js_script_open_handle);

    g_object_unref(fixture->debug_hooks);
    g_object_unref(fixture->context);
}

typedef guint
(*GjsDebugHooksConnectionFunction) (GjsDebugHooks *,
                                    const char    *,
                                    unsigned int   ,
                                    GCallback      ,
                                    gpointer       ,
                                    GError        *);

static void
dummy_callback_for_connector(GjsDebugHooks *hooks,
                             GjsContext    *context,
                             gpointer       data,
                             gpointer       user_data)
{
}

static guint
add_dummy_connection_from_function(GjsDebugHooksFixture            *fixture,
                                   GjsDebugHooksConnectionFunction connector)
{
    return (*connector)(fixture->debug_hooks,
                        fixture->temporary_js_script_filename,
                        0,
                        (GCallback) dummy_callback_for_connector,
                        NULL,
                        NULL);
}

typedef void (*GjsDebugHooksDisconnectorFunction) (GjsDebugHooks *hooks, guint);

typedef struct _TestDebugModeStateData {
    const char                        *componenet_name;
    GjsDebugHooksConnectionFunction   connector;
    GjsDebugHooksDisconnectorFunction disconnector;
} TestDebugModeStateData;

static void
test_debug_mode_on_while_there_are_active_connections(gpointer      fixture_data,
                                                      gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    TestDebugModeStateData *data = (TestDebugModeStateData *) user_data;
    guint connection =
        add_dummy_connection_from_function(fixture, data->connector);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(fixture->context);
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject(js_context));

    g_assert(JS_GetDebugMode(js_context) == JS_TRUE);
    data->disconnector(fixture->debug_hooks, connection);
}

static void
test_debug_mode_off_when_active_connections_are_released(gpointer      fixture_data,
                                                         gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    TestDebugModeStateData *data = (TestDebugModeStateData *) user_data;
    guint connection =
        add_dummy_connection_from_function(fixture, data->connector);
    data->disconnector(fixture->debug_hooks, connection);
    JSContext *js_context = (JSContext *) gjs_context_get_native_context(fixture->context);
    JSAutoCompartment ac(js_context,
                         JS_GetGlobalObject (js_context));

    g_assert (JS_GetDebugMode (js_context) == JS_FALSE);
}

static void
test_fatal_error_when_hook_removed_twice_subprocess(gpointer      fixture_data,
                                                    gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    TestDebugModeStateData *data = (TestDebugModeStateData *) user_data;
    guint connection =
        add_dummy_connection_from_function(fixture, data->connector);

    data->disconnector(fixture->debug_hooks, connection);
    data->disconnector(fixture->debug_hooks, connection);
}

static void
test_fatal_error_when_hook_removed_twice(gpointer      fixture,
                                         gconstpointer user_data)
{
    TestDebugModeStateData *data = (TestDebugModeStateData *) user_data;
    char *child_test_path =
        g_build_filename("/gjs/debug_hooks/fatal_error_on_double_remove/subprocess",
                         data->componenet_name,
                         NULL);
    g_test_trap_subprocess(child_test_path,
                           0,
                           (GTestSubprocessFlags) (G_TEST_SUBPROCESS_INHERIT_STDERR));
    g_test_trap_assert_failed();

    g_free(child_test_path);
}

static void
single_step_mock_interrupt_callback(GjsDebugHooks   *hooks,
                                    GjsContext      *context,
                                    GjsLocationInfo *info,
                                    gpointer         user_data)
{
    unsigned int *hit_count = (unsigned int *) user_data;
    ++(*hit_count);
}

static void
test_interrupts_are_recieved_in_single_step_mode (gpointer      fixture_data,
                                                  gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    unsigned int hit_count = 0;
    guint connection =
        gjs_debug_hooks_add_singlestep_hook(fixture->debug_hooks,
                                            single_step_mock_interrupt_callback,
                                            &hit_count);
    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);
    gjs_debug_hooks_remove_singlestep_hook(fixture->debug_hooks, connection);
    g_assert(hit_count > 0);
}

static void
test_interrupts_are_not_recieved_after_single_step_mode_unlocked (gpointer      fixture_data,
                                                                  gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    unsigned int hit_count = 0;
    guint connection =
        gjs_debug_hooks_add_singlestep_hook(fixture->debug_hooks,
                                            single_step_mock_interrupt_callback,
                                            &hit_count);
    gjs_debug_hooks_remove_singlestep_hook(fixture->debug_hooks, connection);
    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);
    g_assert(hit_count == 0);
}

static gboolean
uint_in_uint_array(unsigned int *array,
                   unsigned int array_len,
                   unsigned int n)
{
    unsigned int i;
    for (i = 0; i < array_len; ++i)
        if (array[i] == n)
            return TRUE;

    return FALSE;
}

static void
single_step_mock_interrupt_callback_tracking_lines (GjsDebugHooks   *hooks,
                                                    GjsContext      *context,
                                                    GjsLocationInfo *info,
                                                    gpointer         user_data)
{
    GArray             *line_tracker = (GArray *) user_data;
    const GjsFrameInfo *frame = gjs_location_info_get_current_frame(info);
    unsigned int line = frame->current_line;

    if (!uint_in_uint_array((unsigned int *) line_tracker->data,
                            line_tracker->len,
                            line))
        g_array_append_val(line_tracker, line);
}

static gboolean
known_executable_lines_are_subset_of_executed_lines(const GArray       *executed_lines,
                                                    const unsigned int *executable_lines,
                                                    gsize               executable_lines_len)
{
    unsigned int i, j;
    for (i = 0; i < executable_lines_len; ++i) {
        gboolean found_executable_line_in_executed_lines = FALSE;
        for (j = 0; j < executed_lines->len; ++j) {
            if (g_array_index (executed_lines, unsigned int, j) == executable_lines[i])
                found_executable_line_in_executed_lines = TRUE;
        }

        if (!found_executable_line_in_executed_lines)
            return FALSE;
    }

    return TRUE;
}

static void
write_content_to_file_at_beginning(int         handle,
                                   const char *content)
{
    if (ftruncate(handle, 0) == -1)
        g_error ("Failed to erase mock file: %s", strerror(errno));
    lseek (handle, 0, SEEK_SET);
    if (write(handle, (gconstpointer) content, strlen(content) * sizeof (char)) == -1)
        g_error ("Failed to write to mock file: %s", strerror(errno));
}

static void
test_interrupts_are_received_on_all_executable_lines_in_single_step_mode (gpointer      fixture_data,
                                                                          gconstpointer user_data)
{
    GArray *line_tracker = g_array_new (FALSE, TRUE, sizeof(unsigned int));
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    guint connection =
        gjs_debug_hooks_add_singlestep_hook(fixture->debug_hooks,
                                            single_step_mock_interrupt_callback_tracking_lines,
                                            line_tracker);
    const char mock_script[] =
        "let a = 1;\n" \
        "let b = 2;\n" \
        "\n" \
        "function func (a, b) {\n" \
        "    let result = a + b;\n" \
        "    return result;\n" \
        "}\n" \
        "\n" \
        "let c = func (a, b);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GjsContext         *reflection_context = gjs_reflected_script_create_reflection_context();
    GjsReflectedScript *reflected = gjs_reflected_script_new(fixture->temporary_js_script_filename,
                                                             reflection_context);
    unsigned int       n_executable_lines = 0;
    const unsigned int *executable_lines =
        gjs_reflected_script_get_expression_lines(reflected, &n_executable_lines);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(known_executable_lines_are_subset_of_executed_lines(line_tracker,
                                                                 executable_lines,
                                                                 n_executable_lines) == TRUE);

    g_array_free(line_tracker, TRUE);
    gjs_debug_hooks_remove_singlestep_hook(fixture->debug_hooks, connection);
    g_object_unref(reflected);
    g_object_unref(reflection_context);
}

static void
mock_breakpoint_callback(GjsDebugHooks   *hooks,
                         GjsContext      *context,
                         GjsLocationInfo *info,
                         gpointer         user_data)
{
    unsigned int *line_hit = (unsigned int *) user_data;
    *line_hit = gjs_location_info_get_current_frame(info)->current_line;
}

static void
test_breakpoint_is_hit_when_adding_before_script_run(gpointer      fixture_data,
                                                     gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "let expected_breakpoint_line = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    unsigned int line_hit = 0;
    guint connection =
        gjs_debug_hooks_add_breakpoint(fixture->debug_hooks,
                                       fixture->temporary_js_script_filename,
                                       1,
                                       mock_breakpoint_callback,
                                       &line_hit);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(line_hit == 1);

    gjs_debug_hooks_remove_breakpoint(fixture->debug_hooks, connection);
}

typedef struct _MockNewScriptHookAddBreakpointData {
    unsigned int connection;
    unsigned int line;
    unsigned int hit_count;
} MockNewScriptHookAddBreakpointData;

static void
mock_new_script_hook_add_breakpoint(GjsDebugHooks      *hooks,
                                    GjsContext         *context,
                                    GjsDebugScriptInfo *info,
                                    gpointer            user_data)
{
    MockNewScriptHookAddBreakpointData *data = (MockNewScriptHookAddBreakpointData *) user_data;
    data->connection = gjs_debug_hooks_add_breakpoint(hooks,
                                                      gjs_debug_script_info_get_filename(info),
                                                      data->line,
                                                      mock_breakpoint_callback,
                                                      &data->hit_count);
}

static void
test_breakpoint_is_hit_when_adding_during_script_run(gpointer      fixture_data,
                                                     gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "let expected_breakpoint_line = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    MockNewScriptHookAddBreakpointData new_script_hook_data = {
        0,
        2,
        0
    };

    guint new_script_hook_connection =
        gjs_debug_hooks_add_script_load_hook(fixture->debug_hooks,
                                             mock_new_script_hook_add_breakpoint,
                                             &new_script_hook_data);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(new_script_hook_data.hit_count > 1);

    gjs_debug_hooks_remove_breakpoint(fixture->debug_hooks, new_script_hook_data.connection);
    gjs_debug_hooks_remove_script_load_hook(fixture->debug_hooks, new_script_hook_connection);
}

static void
test_breakpoint_is_not_hit_when_later_removed (gpointer      fixture_data,
                                               gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "let expected_breakpoint_line = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    unsigned int line_hit = 0;
    guint connection =
        gjs_debug_hooks_add_breakpoint(fixture->debug_hooks,
                                       fixture->temporary_js_script_filename,
                                       1,
                                       mock_breakpoint_callback,
                                       &line_hit);
    gjs_debug_hooks_remove_breakpoint(fixture->debug_hooks, connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(line_hit == 0);
}

static void
mock_frame_execution_interrupt_handler(GjsDebugHooks   *hooks,
                                       GjsContext      *context,
                                       GjsLocationInfo *info,
                                       GjsFrameState   state,
                                       gpointer        user_data)
{
    gboolean *interrupts_received = (gboolean *) user_data;
    *interrupts_received = TRUE;
}

static void
test_interrupts_received_when_connected_to_frame_step(gpointer      fixture_data,
                                                      gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    gboolean interrupts_received = FALSE;

    guint connection =
        gjs_debug_hooks_add_frame_step_hook(fixture->debug_hooks,
                                            mock_frame_execution_interrupt_handler,
                                            &interrupts_received);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(interrupts_received == TRUE);

    gjs_debug_hooks_remove_frame_step_hook(fixture->debug_hooks, connection);
}

static void
mock_frame_execution_interrupt_handler_recording_functions (GjsDebugHooks   *hooks,
                                                            GjsContext      *context,
                                                            GjsLocationInfo *info,
                                                            GjsFrameState    state,
                                                            gpointer         user_data)
{
    const GjsFrameInfo *frame = gjs_location_info_get_current_frame(info);
    GList **function_names_hit = (GList **) user_data;

    *function_names_hit = g_list_append (*function_names_hit,
                                         g_strdup(frame->current_function.function_name));
}

static gboolean
check_if_string_elements_are_in_list (GList       *list,
                                      const char  **elements,
                                      gsize        n_elements)
{
    if (elements && !list)
        return FALSE;

    unsigned int i;
    for (i = 0; i < n_elements; ++i) {
        GList *iter = list;
        gboolean found = FALSE;

        while (iter) {
            if (g_strcmp0 ((const char *) iter->data, elements[i]) == 0) {
                found = TRUE;
                break;
            }

            iter = g_list_next  (iter);
        }

        if (!found)
            return FALSE;
    }

    return TRUE;
}

static void
test_expected_function_names_hit_on_frame_step (gpointer      fixture_data,
                                                gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "function foo (a) {\n"
        "    return a;\n"
        "}\n"
        "let b = foo (a);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GList *function_names_hit = NULL;
    guint connection =
        gjs_debug_hooks_add_frame_step_hook(fixture->debug_hooks,
                                            mock_frame_execution_interrupt_handler_recording_functions,
                                            &function_names_hit);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    const char *expected_function_names_hit[] = {
      "foo"
    };
    const gsize expected_function_names_hit_len =
        G_N_ELEMENTS(expected_function_names_hit);

    g_assert(check_if_string_elements_are_in_list(function_names_hit,
                                                  expected_function_names_hit,
                                                  expected_function_names_hit_len));

    if (function_names_hit)
        g_list_free_full(function_names_hit, g_free);

    gjs_debug_hooks_remove_frame_step_hook(fixture->debug_hooks,
                                           connection);
}

static void
test_nothing_hit_when_frame_step_hook_removed (gpointer      fixture_data,
                                               gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char mock_script[] =
        "let a = 1;\n"
        "function foo (a) {\n"
        "    return a;\n"
        "}\n"
        "let b = foo (a);\n"
        "\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       mock_script);

    GList *function_names_hit = NULL;
    guint connection =
        gjs_debug_hooks_add_frame_step_hook(fixture->debug_hooks,
                                            mock_frame_execution_interrupt_handler_recording_functions,
                                            &function_names_hit);
    gjs_debug_hooks_remove_frame_step_hook(fixture->debug_hooks, connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(function_names_hit == NULL);
}

static void
replace_string(char       **string_pointer,
               const char *new_string)
{
    if (*string_pointer)
        g_free (*string_pointer);

    *string_pointer = g_strdup (new_string);
}

static void
mock_new_script_hook(GjsDebugHooks      *hooks,
                     GjsContext         *context,
                     GjsDebugScriptInfo *info,
                     gpointer            user_data)
{
    char **last_loaded_script = (char **) user_data;

    replace_string(last_loaded_script,
                   gjs_debug_script_info_get_filename(info));
}

static void
test_script_load_notification_sent_on_new_script(gpointer      fixture_data,
                                                 gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char loadable_script[] = "let a = 1;\n\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       loadable_script);

    char *last_loaded_script = NULL;
    guint connection =
        gjs_debug_hooks_add_script_load_hook(fixture->debug_hooks,
                                             mock_new_script_hook,
                                             &last_loaded_script);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(last_loaded_script != NULL &&
             g_strcmp0(last_loaded_script,
                       fixture->temporary_js_script_filename) == 0);

    g_free(last_loaded_script);
    gjs_debug_hooks_remove_script_load_hook(fixture->debug_hooks, connection);
}

static void
test_script_load_notification_not_sent_on_connection_removed(gpointer      fixture_data,
                                                             gconstpointer user_data)
{
    GjsDebugHooksFixture *fixture = (GjsDebugHooksFixture *) fixture_data;
    const char loadable_script[] = "let a = 1;\n\n";

    write_content_to_file_at_beginning(fixture->temporary_js_script_open_handle,
                                       loadable_script);

    char *last_loaded_script = NULL;
    guint connection =
        gjs_debug_hooks_add_script_load_hook(fixture->debug_hooks,
                                             mock_new_script_hook,
                                             &last_loaded_script);

    gjs_debug_hooks_remove_script_load_hook(fixture->debug_hooks, connection);

    gjs_context_eval_file(fixture->context,
                          fixture->temporary_js_script_filename,
                          NULL,
                          NULL);

    g_assert(last_loaded_script == NULL);
}

typedef void (*TestDataFunc)(gpointer      data,
                             gconstpointer user_data);

static void
for_each_in_table_driven_test_data(gconstpointer test_data,
                                   gsize         element_size,
                                   gsize         n_elements,
                                   TestDataFunc  func,
                                   gconstpointer user_data)
{
    const char *test_data_iterator = (const char *) test_data;
    gsize i;
    for (i = 0; i < n_elements; ++i, test_data_iterator += element_size)
        (*func)((char *) (test_data_iterator), user_data);
}

typedef struct _FixturedTest {
    gsize            fixture_size;
    GTestFixtureFunc set_up;
    GTestFixtureFunc tear_down;
} FixturedTest;

static void
add_test_for_fixture(const char       *name,
                     FixturedTest     *fixture,
                     GTestFixtureFunc test_func,
                     gconstpointer    user_data)
{
    g_test_add_vtable(name,
                      fixture->fixture_size,
                      user_data,
                      fixture->set_up,
                      test_func,
                      fixture->tear_down);
}

typedef struct _FixturedTableDrivenTestData {
    const char       *test_name;
    gsize            fixture_size;
    GTestFixtureFunc set_up;
    GTestFixtureFunc test_func;
    GTestFixtureFunc tear_down;
} FixturedTableDrivenTestData;

static void
add_test_for_fixture_size_and_funcs(gpointer      data,
                                    gconstpointer user_data)
{
    const FixturedTableDrivenTestData *fixtured_table_driven_test = (const FixturedTableDrivenTestData *) data;
    FixturedTest fixtured_test = {
        fixtured_table_driven_test->fixture_size,
        fixtured_table_driven_test->set_up,
        fixtured_table_driven_test->tear_down
    };
    add_test_for_fixture(fixtured_table_driven_test->test_name,
                         &fixtured_test,
                         fixtured_table_driven_test->test_func,
                         user_data);

}

typedef struct _GjsDebugHooksTableDrivenTest {
    const char      *prefix;
    GTestFixtureFunc test_function;
} GjsDebugHooksTableDrivenTest;

static void
add_gjs_debug_hooks_context_state_data_test(gpointer      data,
                                                        gconstpointer user_data)
{
    const GjsDebugHooksTableDrivenTest *test = (const GjsDebugHooksTableDrivenTest *) user_data;
    TestDebugModeStateData             *table_data = (TestDebugModeStateData *) data;

    char *test_name = g_strconcat(test->prefix, "/", table_data->componenet_name, NULL);

    FixturedTableDrivenTestData fixtured_data = {
        test_name,
        sizeof(GjsDebugHooksFixture),
        gjs_debug_hooks_fixture_set_up,
        test->test_function,
        gjs_debug_hooks_fixture_tear_down
    };

    add_test_for_fixture_size_and_funcs(&fixtured_data,
                                        (gconstpointer) table_data);

    g_free (test_name);
}

void gjs_test_add_tests_for_debug_hooks()
{
    static const TestDebugModeStateData context_state_data[] = {
        {
            "add_breakpoint",
            (GjsDebugHooksConnectionFunction) gjs_debug_hooks_add_breakpoint,
            gjs_debug_hooks_remove_breakpoint
        },
        {
            "singlestep",
            (GjsDebugHooksConnectionFunction) gjs_debug_hooks_add_singlestep_hook,
            gjs_debug_hooks_remove_singlestep_hook
        },
        {
            "script_load",
            (GjsDebugHooksConnectionFunction) gjs_debug_hooks_add_script_load_hook,
            gjs_debug_hooks_remove_script_load_hook
        },
        {
            "hook_frame_step",
            (GjsDebugHooksConnectionFunction) gjs_debug_hooks_add_frame_step_hook,
            gjs_debug_hooks_remove_frame_step_hook
        }
    };
    const gsize context_state_data_len =
        G_N_ELEMENTS(context_state_data);

    const GjsDebugHooksTableDrivenTest debug_hooks_tests_info[] = {
        {
            "/gjs/debug_hooks/debug_mode_on_for",
            test_debug_mode_on_while_there_are_active_connections
        },
        {
            "/gjs/debug_hooks/debug_mode_off_when_released",
            test_debug_mode_off_when_active_connections_are_released
        },
        {
            "/gjs/debug_hooks/fatal_error_on_double_remove",
            test_fatal_error_when_hook_removed_twice
        },
        {
            "/gjs/debug_hooks/fatal_error_on_double_remove/subprocess",
            test_fatal_error_when_hook_removed_twice_subprocess
        }
    };
    const gsize debug_hooks_tests_info_size =
        G_N_ELEMENTS(debug_hooks_tests_info);

    gsize i;
    for (i = 0; i < debug_hooks_tests_info_size; ++i)
        for_each_in_table_driven_test_data(&context_state_data,
                                           sizeof(TestDebugModeStateData),
                                           context_state_data_len,
                                           add_gjs_debug_hooks_context_state_data_test,
                                           (gconstpointer) &debug_hooks_tests_info[i]);

    FixturedTest debug_hooks_fixture = {
        sizeof (GjsDebugHooksFixture),
        gjs_debug_hooks_fixture_set_up,
        gjs_debug_hooks_fixture_tear_down
    };

    add_test_for_fixture("/gjs/debug_hooks/interrupts_recieved_when_in_single_step_mode",
                         &debug_hooks_fixture,
                         test_interrupts_are_recieved_in_single_step_mode,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/no_interrupts_after_singlestep_removed",
                         &debug_hooks_fixture,
                         test_interrupts_are_not_recieved_after_single_step_mode_unlocked,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/interrupts_received_on_expected_lines_of_script",
                         &debug_hooks_fixture,
                         test_interrupts_are_received_on_all_executable_lines_in_single_step_mode,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/breakpoint_hit_when_added_before_script_run",
                         &debug_hooks_fixture,
                         test_breakpoint_is_hit_when_adding_before_script_run,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/breakpoint_hit_when_added_during_script_run",
                         &debug_hooks_fixture,
                         test_breakpoint_is_hit_when_adding_during_script_run,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/breakpoint_not_hit_after_removal",
                         &debug_hooks_fixture,
                         test_breakpoint_is_not_hit_when_later_removed,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/interrupts_on_frame_execution",
                         &debug_hooks_fixture,
                         test_interrupts_received_when_connected_to_frame_step,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/interrupts_for_expectected_on_frame_execution",
                         &debug_hooks_fixture,
                         test_expected_function_names_hit_on_frame_step,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/no_interrupts_on_frame_execution_removed",
                         &debug_hooks_fixture,
                         test_nothing_hit_when_frame_step_hook_removed,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/new_script_notification",
                         &debug_hooks_fixture,
                         test_script_load_notification_sent_on_new_script,
                         NULL);
    add_test_for_fixture("/gjs/debug_hooks/no_script_notification_on_hook_removed",
                         &debug_hooks_fixture,
                         test_script_load_notification_not_sent_on_connection_removed,
                         NULL);
}
