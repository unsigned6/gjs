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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <gjs/gjs.h>
#include <gjs/reflected-script.h>

typedef struct _GjsReflectedScriptTestFixture {
    char       *temporary_js_script_filename;
    int        temporary_js_script_open_handle;
    GjsContext *reflection_context;
} GjsReflectedScriptTestFixture;

static void
gjs_reflected_test_fixture_set_up(gpointer      fixture_data,
                                  gconstpointer test_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;
    char                          *current_dir = g_get_current_dir();
    fixture->temporary_js_script_open_handle = g_file_open_tmp("mock-js-XXXXXXX.js",
                                                               &fixture->temporary_js_script_filename,
                                                               NULL);
    fixture->reflection_context = gjs_reflected_script_create_reflection_context();

    g_free(current_dir);
}

static void
gjs_reflected_test_fixture_tear_down(gpointer      fixture_data,
                                     gconstpointer test_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;
    unlink(fixture->temporary_js_script_filename);
    g_free(fixture->temporary_js_script_filename);
    close(fixture->temporary_js_script_open_handle);

    g_object_unref(fixture->reflection_context);
}

static void
test_reflect_creation_and_destruction(gpointer      fixture_data,
                                      gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;

    const char mock_script[] = "var a = 1;\n";

    if (write(fixture->temporary_js_script_open_handle, mock_script, strlen(mock_script) * sizeof(char)) == -1)
        g_error("Failed to write to test script");

    GjsReflectedScript *script = gjs_reflected_script_new(fixture->temporary_js_script_filename,
                                                          fixture->reflection_context);
    g_object_unref(script);
}

static gboolean
integer_arrays_equal(const unsigned int *actual,
                     unsigned int        actual_n,
                     const unsigned int *expected,
                     unsigned int        expected_n)
{
    if (actual_n != expected_n)
        return FALSE;

    unsigned int i;

    for (i = 0; i < actual_n; ++i)
        if (actual[i] != expected[i])
            return FALSE;

    return TRUE;
}

static GjsReflectedScript *
get_reflected_script_for(const char *script,
                         int         file,
                         const char *filename,
                         GjsContext *reflection_context)
{
    if (write(file, script, strlen(script) * sizeof(char)) == -1)
        g_error("Failed to write to test script");

    GjsReflectedScript *reflected_script =
        gjs_reflected_script_new(filename, reflection_context);
    return reflected_script;
}

static void
test_reflect_get_all_executable_expression_lines(gpointer      fixture_data,
                                                 gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;

    const char mock_script[] =
            "var a = 1.0;\n"
            "var b = 2.0;\n"
            "var c = 3.0;\n";

    GjsReflectedScript *script = get_reflected_script_for(mock_script,
                                                          fixture->temporary_js_script_open_handle,
                                                          fixture->temporary_js_script_filename,
                                                          fixture->reflection_context);
    unsigned int       n_executable_lines = 0;
    const unsigned int *executable_lines = gjs_reflected_script_get_expression_lines(script,
                                                                                     &n_executable_lines);

    const unsigned int expected_executable_lines[] = {
        1, 2, 3
    };
    const unsigned int n_expected_executable_lines = G_N_ELEMENTS(expected_executable_lines);

    g_assert(integer_arrays_equal(executable_lines,
                                  n_executable_lines,
                                  expected_executable_lines,
                                  n_expected_executable_lines));

    g_object_unref(script);
}

typedef struct _GjsReflectedScriptExpectedBranch {
    unsigned int point;
    unsigned int alternatives[256];
    unsigned int n_alternatives;
} ExpectedBranch;

static gboolean
branch_info_equal(ExpectedBranch                     *expected,
                  const GjsReflectedScriptBranchInfo *branch)
{
    if (gjs_reflected_script_branch_info_get_branch_point(branch) != expected->point)
        return FALSE;

    unsigned int       n_actual_alternatives = 0;
    const unsigned int *actual_alternatives = gjs_reflected_script_branch_info_get_branch_alternatives(branch,
                                                                                                       &n_actual_alternatives);
    unsigned int       n_expected_alternatives = expected->n_alternatives;
    const unsigned int *expected_alternatives = expected->alternatives;

    return integer_arrays_equal(actual_alternatives,
                                n_actual_alternatives,
                                expected_alternatives,
                                n_expected_alternatives);
}

static gboolean
has_elements_in_branch_array_in_order (ExpectedBranch                     *expected,
                                       const GjsReflectedScriptBranchInfo **branches,
                                       unsigned int                        n_expected)
{
    const GjsReflectedScriptBranchInfo **branch_iterator = branches;
    unsigned int i;

    for (i = 0; i < n_expected; ++i, ++branch_iterator) {
        /* Size mismatch */
        if (!(*branch_iterator))
            return FALSE;

        if (!branch_info_equal(&expected[i],
                               *branch_iterator))
            return FALSE;
    }

    /* Size mismatch - there are still remaining branches */
    if (*branch_iterator)
        return FALSE;

    return TRUE;
}

static void
test_reflect_finds_branches(gpointer      fixture_data,
                            gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;

    const char mock_script[] =
        "let a, b;\n"
        "if (1)\n"
        "    a = 1.0\n"
        "else\n"
        "    b = 2.0\n"
        "\n";

    GjsReflectedScript *script = get_reflected_script_for(mock_script,
                                                          fixture->temporary_js_script_open_handle,
                                                          fixture->temporary_js_script_filename,
                                                          fixture->reflection_context);
    const GjsReflectedScriptBranchInfo **branches =
        gjs_reflected_script_get_branches(script);

    ExpectedBranch expected[] = {
        {
            2,
            { 3, 5 },
            2
        }
    };

    g_assert(has_elements_in_branch_array_in_order(expected,
                                                   branches,
                                                   G_N_ELEMENTS(expected)));

    g_object_unref(script);
}

typedef struct _ExpectedReflectedFunction {
    unsigned int line;
    unsigned int n_params;
    const char   *name;
} ExpectedReflectedFunction;

static gboolean
function_info_equal(const ExpectedReflectedFunction      *expected,
                    const GjsReflectedScriptFunctionInfo *actual)
{
    const gchar  *actual_name = gjs_reflected_script_function_info_get_name(actual);
    unsigned int actual_line = gjs_reflected_script_function_info_get_line_number(actual);
    unsigned int actual_n_params = gjs_reflected_script_function_info_get_n_params(actual);

    if ((actual_name && !expected->name) ||
        (!actual_name && expected->name))
        return FALSE;

    if (g_strcmp0(actual_name, expected->name) != 0)
        return FALSE;

    return actual_line == expected->line &&
           actual_n_params == expected->n_params;
}

static gboolean
has_elements_in_function_array_in_order(const GjsReflectedScriptFunctionInfo **functions,
                                        const ExpectedReflectedFunction      *expected,
                                        unsigned int                          n_expected)
{
    const GjsReflectedScriptFunctionInfo **function_iterator = functions;
    unsigned int i;

    for (i = 0; i < n_expected; ++i, ++function_iterator) {
        /* Size mismatch */
        if (!(*function_iterator))
            return FALSE;

        if (!function_info_equal(&expected[i],
                                 *function_iterator))
            return FALSE;
    }

    /* Size mismatch - there are still remaining functions */
    if (*function_iterator)
        return FALSE;

    return TRUE;
}

static void
test_reflect_finds_functions(gpointer      fixture_data,
                             gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;

    const char mock_script[] =
        "function f1() {}\n"
        "function f2() {}\n"
        "function f3() {}\n";

    GjsReflectedScript *script = get_reflected_script_for(mock_script,
                                                          fixture->temporary_js_script_open_handle,
                                                          fixture->temporary_js_script_filename,
                                                          fixture->reflection_context);
    const GjsReflectedScriptFunctionInfo **functions =
        gjs_reflected_script_get_functions(script);

    ExpectedReflectedFunction expected[] = {
        { 1, 0, "f1" },
        { 2, 0, "f2" },
        { 3, 0, "f3" }
    };

    g_assert(has_elements_in_function_array_in_order(functions,
                                                     expected,
                                                     G_N_ELEMENTS(expected)));

    g_object_unref(script);
}

static void
test_reflect_get_n_lines(gpointer      fixture_data,
                         gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture = (GjsReflectedScriptTestFixture *) fixture_data;

    const char mock_script[] =
        "function f1() {}\n"
        "function f2() {}\n"
        "function f3() {}\n";

    GjsReflectedScript *script = get_reflected_script_for(mock_script,
                                                          fixture->temporary_js_script_open_handle,
                                                          fixture->temporary_js_script_filename,
                                                          fixture->reflection_context);
    unsigned int n_lines = gjs_reflected_script_get_n_lines(script);

    g_assert(n_lines == 4);

    g_object_unref(script);
}

static void
silence_log_handler(const char     *log_domain,
                    GLogLevelFlags  flags,
                    const char     *message,
                    gpointer        user_data)
{
}

static void
test_reflect_on_nonexistent_script_returns_empty(gpointer      fixture_data,
                                                 gconstpointer user_data)
{
    GjsReflectedScriptTestFixture *fixture =
        (GjsReflectedScriptTestFixture *) fixture_data;
    GjsReflectedScript *script = gjs_reflected_script_new("doesnotexist://does_not_exist",
                                                          fixture->reflection_context);

    /* Make the handler shut up so that we don't get an assertion on
     * raised warnings from bad scripts */
    GLogLevelFlags old_flags = g_log_set_always_fatal((GLogLevelFlags) G_LOG_LEVEL_ERROR);
    GLogFunc old_handler = g_log_set_default_handler(silence_log_handler, NULL);

    const GjsReflectedScriptFunctionInfo **functions =
        gjs_reflected_script_get_functions(script);
    const GjsReflectedScriptBranchInfo **branches =
        gjs_reflected_script_get_branches(script);
    unsigned int n_expression_lines;
    const unsigned int *lines =
        gjs_reflected_script_get_expression_lines(script, &n_expression_lines);
    unsigned int n_lines = gjs_reflected_script_get_n_lines(script);

    g_assert(*functions == NULL);
    g_assert(*branches == NULL);
    g_assert(*lines == 0);
    g_assert(n_lines == 0);
    g_assert(n_expression_lines == 0);

    g_log_set_default_handler(old_handler, NULL);
    g_log_set_always_fatal(old_flags);
}

typedef struct _TestFixture
{
    GTestFixtureFunc set_up;
    GTestFixtureFunc tear_down;
    size_t           test_fixture_size;
} TestFixture;

static void
add_test_for_fixture(const char       *name,
                     TestFixture      *fixture,
                     GTestFixtureFunc  test_func)
{
    g_test_add_vtable(name,
                      fixture->test_fixture_size,
                      NULL,
                      fixture->set_up,
                      test_func,
                      fixture->tear_down);
}

void
gjs_test_add_tests_for_reflected_script(void)
{
    TestFixture reflected_script_default_fixture = {
        gjs_reflected_test_fixture_set_up,
        gjs_reflected_test_fixture_tear_down,
        sizeof(GjsReflectedScriptTestFixture)
    };

    add_test_for_fixture("/gjs/reflected_script/construction",
                         &reflected_script_default_fixture,
                         test_reflect_creation_and_destruction);
    add_test_for_fixture("/gjs/reflected_script/all_are_expression_lines",
                         &reflected_script_default_fixture,
                         test_reflect_get_all_executable_expression_lines);
    add_test_for_fixture("/gjs/reflected_script/finds_branches",
                         &reflected_script_default_fixture,
                         test_reflect_finds_branches);
    add_test_for_fixture("/gjs/reflected_script/finds_functions",
                         &reflected_script_default_fixture,
                         test_reflect_finds_functions);
    add_test_for_fixture("/gjs/reflected_script/n_lines",
                         &reflected_script_default_fixture,
                         test_reflect_get_n_lines);
    add_test_for_fixture("/gjs/reflected/script/nonexistent",
                         &reflected_script_default_fixture,
                         test_reflect_on_nonexistent_script_returns_empty);
}
