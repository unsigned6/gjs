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
#include <stdio.h>
#include <string.h>

#include <fcntl.h>

#include <gio/gio.h>
#include <gjs/gjs.h>
#include <gjs/debug-hooks.h>
#include <gjs/reflected-script.h>
#include <gjs/coverage.h>

typedef struct _GjsCoverageBranchData GjsCoverageBranchData;

struct _GjsCoveragePrivate {
    GHashTable    *file_statistics;
    GjsDebugHooks *debug_hooks;
    gchar         **covered_paths;

    /* A separate context where reflection is performed. We don't
     * want to use the main context because we don't want to
     * modify its state while it is being debugged.
     *
     * A single context is shared across all reflections because
     * the reflection functions are effectively const.
     *
     * This is created on-demand when debugging is enabled. GjsContext
     * creates an instance of us by default so we obviously don't want
     * to recurse into creating an instance of GjsContext by default.
     */
    GjsContext *reflection_context;

    guint         new_scripts_connection;
    guint         single_step_connection;
    guint         frame_step_connection;

    /* If we hit a branch and the next single-step line will
     * activate one of the branch alternatives then this will
     * be set to that branch
     *
     * XXX: This isn't necessarily safe in the presence of
     * multiple execution contexts which are connected
     * to the same GjsCoveragePrivate's single step hook */
    GjsCoverageBranchData *active_branch;
};

G_DEFINE_TYPE_WITH_PRIVATE(GjsCoverage,
                           gjs_coverage,
                           G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_DEBUG_HOOKS,
    PROP_COVERAGE_PATHS,
    PROP_N
};

static GParamSpec *properties[PROP_N] = { NULL, };

struct _GjsCoverageBranchData {
    GArray       *branch_alternatives;
    GArray       *branch_alternatives_taken;
    unsigned int branch_point;
    unsigned int last_branch_exit;
    gboolean     branch_hit;
};

static unsigned int
determine_highest_unsigned_int(GArray *array)
{
    unsigned int highest = 0;
    unsigned int i = 0;

    for (; i < array->len; ++i) {
        unsigned int value = g_array_index(array, unsigned int, i);
        if (highest < value)
            highest = value;
    }

    return highest;
}

static void
gjs_coverage_branch_info_init(GjsCoverageBranchData              *data,
                              const GjsReflectedScriptBranchInfo *info)
{
    g_assert(data->branch_alternatives == NULL);
    g_assert(data->branch_alternatives_taken == NULL);
    g_assert(data->branch_point == 0);
    g_assert(data->last_branch_exit == 0);
    g_assert(data->branch_hit == 0);

    unsigned int n_branches;
    const unsigned int *alternatives =
        gjs_reflected_script_branch_info_get_branch_alternatives(info, &n_branches);

    /* We need to copy the alternatives as there's a case where we might outlive
     * the reflected script.
     *
     * Another potential option here would be to expose the GArray or
     * GjsReflectedScript here. However, it doesn't make sense for this structure
     * to have ownership of either */
    data->branch_alternatives = g_array_sized_new(FALSE, TRUE, sizeof(unsigned int), n_branches);
    g_array_set_size(data->branch_alternatives, n_branches);

    memcpy(data->branch_alternatives->data,
           alternatives,
           sizeof(unsigned int) * n_branches);

    data->branch_alternatives_taken =
        g_array_new(FALSE, TRUE, sizeof(unsigned int));
    g_array_set_size(data->branch_alternatives_taken, n_branches);
    data->branch_point = gjs_reflected_script_branch_info_get_branch_point(info);
    data->last_branch_exit = determine_highest_unsigned_int(data->branch_alternatives);
    data->branch_hit = FALSE;
}

static void
gjs_coverage_branch_info_clear(gpointer data_ptr)
{
    GjsCoverageBranchData *data = (GjsCoverageBranchData *) data_ptr;

    if (data->branch_alternatives_taken) {
        g_array_unref(data->branch_alternatives_taken);
        data->branch_alternatives_taken = NULL;
    }

    if (data->branch_alternatives) {
        g_array_unref(data->branch_alternatives);
        data->branch_alternatives = NULL;
    }
}

static char *
create_function_lookup_key(const gchar  *name,
                           unsigned int  line,
                           unsigned int  n_param)
{
    return g_strdup_printf("%s:%i:%i",
                           name ? name : "(anonymous)",
                           line,
                           n_param);
}

typedef struct _GjsCoverageFileStatistics {
    /* 1-1 with line numbers for O(N) lookup */
    GArray     *lines;
    GArray     *branches;

    /* Hash buckets for O(logn) lookup */
    GHashTable *functions;
} GjsCoverageFileStatistics;

GjsCoverageFileStatistics *
gjs_coverage_file_statistics_new(GArray *all_lines,
                                 GArray *all_branches,
                                 GHashTable *all_functions)
{
    GjsCoverageFileStatistics *file_stats = g_new0(GjsCoverageFileStatistics, 1);
    file_stats->lines = all_lines;
    file_stats->branches = all_branches;
    file_stats->functions = all_functions;
    return file_stats;
}

void
gjs_coverage_file_statistics_destroy(gpointer data)
{
    GjsCoverageFileStatistics *file_stats = (GjsCoverageFileStatistics *) data;
    g_array_unref(file_stats->lines);
    g_array_unref(file_stats->branches);
    g_hash_table_unref(file_stats->functions);
    g_free(file_stats);
}

static void
increment_line_hits(GArray       *line_counts,
                    unsigned int  line_no)
{
    g_assert(line_no <= line_counts->len);

    int *line_hit_count = &(g_array_index(line_counts, int, line_no));

    /* If this happens it is not a huge problem - though it does
     * mean that infoReflect.js is not doing its job, so we should
     * print a debug message about it in case someone is interested.
     *
     * The reason why we don't have a proper warning is because it
     * is difficult to determine what the SpiderMonkey program counter
     * will actually pass over, especially function declarations for some
     * reason:
     *
     *     function f(a,b) {
     *         a = 1;
     *     }
     *
     * In some cases, the declaration itself will be executed
     * but in other cases it won't be. Reflect.parse tells us that
     * the only two expressions on that line are a FunctionDeclaration
     * and BlockStatement, neither of which would ordinarily be
     * executed */
    if (*line_hit_count == -1) {
        g_debug("Executed line %i which we thought was not executable", line_no);
        *line_hit_count = 0;
    }

    ++(*line_hit_count);
}

static void
increment_hits_on_branch(GjsCoverageBranchData *branch,
                         unsigned int           line)
{
    if (!branch)
        return;

    g_assert (branch->branch_alternatives->len == branch->branch_alternatives_taken->len);

    unsigned int i;
    for (i = 0; i < branch->branch_alternatives->len; ++i) {

        if (g_array_index (branch->branch_alternatives, unsigned int, i) == line) {
            unsigned int *hit_count = &(g_array_index(branch->branch_alternatives_taken,
                                                      unsigned int,
                                                      i));
            ++(*hit_count);
        }
    }
}

/* Return a valid GjsCoverageBranchData if this line actually
 * contains a valid branch (eg GjsReflectedScriptBranchInfo is set) */
static GjsCoverageBranchData *
find_active_branch(GArray                *branches,
                   unsigned int           line,
                   GjsCoverageBranchData *active_branch)
{
    g_assert(line <= branches->len);

    GjsCoverageBranchData *branch = &(g_array_index(branches, GjsCoverageBranchData, line));
    if (branch->branch_point) {
        branch->branch_hit = TRUE;
        return branch;
    }

    /* We shouldn't return NULL until we're actually outside the
     * active branch, since we might be in a case statement where
     * we need to check every possible option before jumping to an
     * exit */
    if (active_branch) {
        if (line <= active_branch->last_branch_exit)
            return active_branch;

        return NULL;
    }

    return NULL;
}

static void
gjs_coverage_single_step_interrupt_hook(GjsDebugHooks   *hooks,
                                        GjsContext      *context,
                                        GjsLocationInfo *info,
                                        gpointer         user_data)
{
    GjsCoverage *coverage = (GjsCoverage *) user_data;
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    const GjsFrameInfo *frame = gjs_location_info_get_current_frame(info);

    const char *filename = frame->current_function.filename;
    unsigned int line_no = frame->current_line;
    GHashTable  *statistics_table = priv->file_statistics;
    GjsCoverageFileStatistics *statistics =
        (GjsCoverageFileStatistics *) g_hash_table_lookup(statistics_table,
                                                          filename);
    /* We don't care about this file, even if we're single-stepping it */
    if (!statistics)
        return;

    /* Line counters */
    increment_line_hits(statistics->lines, line_no);

    /* Branch counters. First increment branch hits for the active
     * branch and then find a new potentially active branch */
    increment_hits_on_branch(priv->active_branch, line_no);
    priv->active_branch = find_active_branch(statistics->branches,
                                             line_no,
                                             priv->active_branch);
}

static void
gjs_coverage_frame_execution_hook(GjsDebugHooks   *hooks,
                                  GjsContext      *context,
                                  GjsLocationInfo *info,
                                  GjsFrameState    state,
                                  gpointer         user_data)
{
    /* We don't care about after-hits */
    if (state != GJS_FRAME_ENTRY)
        return;

    const GjsFrameInfo *frame = gjs_location_info_get_current_frame(info);
    const char *function_name = frame->current_function.function_name;
    unsigned int line = frame->current_function.line;
    unsigned int n_params = frame->current_function.n_args;

    GHashTable *all_statistics = (GHashTable *) user_data;
    GjsCoverageFileStatistics *file_statistics =
        (GjsCoverageFileStatistics *) g_hash_table_lookup(all_statistics,
                                                          frame->current_function.filename);

    /* We don't care about this script */
    if (!file_statistics)
        return;

    /* Not a function, so we don't care */
    if (!function_name)
        return;

    /* If you have a function name longer than this then there's a real problem.
     * We are using the stack here since this is a rather hot path and allocation
     * is something that we should avoid doing here */
    gchar key_buffer[1024];
    if (snprintf(key_buffer,
                 1024,
                 "%s:%i:%i",
                 function_name ? function_name : "f",
                 line,
                 n_params) >= 1024) {
        g_warning("Failed to create function key, the function name %s is too long!",
                   function_name ? function_name : "f");
        return;
    }

    if (!g_hash_table_contains(file_statistics->functions, key_buffer)) {
        g_debug("Entered unknown function %s:%i:%i",
                function_name,
                line,
                n_params);
    }

    unsigned int hit_count = GPOINTER_TO_INT(g_hash_table_lookup(file_statistics->functions, function_name));
    ++hit_count;

    /* The GHashTable API requires that we copy the key again, in both the
     * insert and replace case */
    g_hash_table_replace(file_statistics->functions,
                         g_strdup(key_buffer),
                         GINT_TO_POINTER(hit_count));
}

/*
 * The created array is a 1-1 representation of the hitcount in the filename. Each
 * element refers to an individual line. In order to avoid confusion, our array
 * is zero indexed, but the zero'th line is always ignored and the first element
 * refers to the first line of the file.
 *
 * A value of -1 for an element means that the line is non-executable and never actually
 * reached. A value of 0 means that it was executable but never reached. A positive value
 * indicates the hit count.
 *
 * We care about non-executable lines because we don't want to report coverage misses for
 * lines that could have never been executed anyways.
 *
 * The reason for using a 1-1 mapping as opposed to an array of key-value pairs for executable
 * lines is:
 *   1. Lookup speed is O(1) instead of O(log(n))
 *   2. There's a possibility we might hit a line which we thought was non-executable, in which
 *      case we can neatly handle the error by marking that line executable. A hit on a line
 *      we thought was non-executable is not as much of a problem as noise generated by
 *      ostensible "misses" which could in fact never be executed.
 *
 */
static GArray *
create_line_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    unsigned int line_count = gjs_reflected_script_get_n_lines(reflected_script);
    GArray *line_statistics = g_array_new(TRUE, FALSE, sizeof(int));

    /* We are ignoring the zeroth line, so we want line_count + 1 */
    g_array_set_size(line_statistics, line_count + 1);

    if (line_count)
        memset(line_statistics->data, -1, sizeof(int) * line_statistics->len);

    unsigned int       n_expression_lines;
    const unsigned int *executable_lines =
        gjs_reflected_script_get_expression_lines(reflected_script,
                                                  &n_expression_lines);

    /* In order to determine which lines are executable to start off with, we take
     * the array of executable lines provided to us with gjs_debug_script_info_get_executable_lines
     * and change the array value of each line to zero. If these lines are never executed then
     * they will be considered a coverage miss */
    if (executable_lines) {
        unsigned int i;
        for (i = 0; i < n_expression_lines; ++i)
            g_array_index(line_statistics, int, executable_lines[i]) = 0;
    }

    return line_statistics;
}

/* As above, we are creating a 1-1 representation of script lines to potential branches
 * where each element refers to a 1-index line (with the zero'th ignored).
 *
 * Each element is a GjsCoverageBranchData which, if the line at the element
 * position describes a branch, will be populated with a GjsReflectedScriptBranchInfo
 * and an array of unsigned each specifying the hit-count for each potential branch
 * in the branch info */
static GArray *
create_branch_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    unsigned int line_count = gjs_reflected_script_get_n_lines(reflected_script);
    GArray *branch_statistics = g_array_new(FALSE, TRUE, sizeof(GjsCoverageBranchData));
    g_array_set_size(branch_statistics, line_count + 1);
    g_array_set_clear_func(branch_statistics, gjs_coverage_branch_info_clear);

    const GjsReflectedScriptBranchInfo **branch_info_iterator =
        gjs_reflected_script_get_branches(reflected_script);

    if (*branch_info_iterator) {
        do {
            unsigned int branch_point =
                gjs_reflected_script_branch_info_get_branch_point(*branch_info_iterator);

            g_assert(branch_point <= branch_statistics->len);

            GjsCoverageBranchData *branch_data (&(g_array_index(branch_statistics,
                                                                GjsCoverageBranchData,
                                                                branch_point)));
            gjs_coverage_branch_info_init(branch_data, *branch_info_iterator);
        } while (*(++branch_info_iterator));
    }

    return branch_statistics;
}

static GHashTable *
create_function_coverage_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    GHashTable *functions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const GjsReflectedScriptFunctionInfo **function_info_iterator =
        gjs_reflected_script_get_functions(reflected_script);

    if (*function_info_iterator) {
        do {
            const char   *name = gjs_reflected_script_function_info_get_name(*function_info_iterator);
            unsigned int line = gjs_reflected_script_function_info_get_line_number(*function_info_iterator);
            unsigned int n_params = gjs_reflected_script_function_info_get_n_params(*function_info_iterator);

            char *key = create_function_lookup_key(name, line, n_params);
            g_hash_table_insert(functions, key, GINT_TO_POINTER(0));
        } while (*(++function_info_iterator));
    }

    return functions;
}

static GjsCoverageFileStatistics *
create_statistics_from_reflection(GjsReflectedScript *reflected_script)
{
    GArray *line_coverage_statistics =
        create_line_coverage_statistics_from_reflection(reflected_script);
    GArray *branch_coverage_statistics =
        create_branch_coverage_statistics_from_reflection(reflected_script);
    GHashTable *function_coverage_statistics =
        create_function_coverage_statistics_from_reflection(reflected_script);

    g_assert(line_coverage_statistics);
    g_assert(branch_coverage_statistics);
    g_assert(function_coverage_statistics);

    return gjs_coverage_file_statistics_new(line_coverage_statistics,
                                            branch_coverage_statistics,
                                            function_coverage_statistics);

}

static GjsCoverageFileStatistics *
new_statistics_for_filename(GjsContext *reflection_context,
                            const char *filename)
{
    GjsReflectedScript *reflected_script =
        gjs_reflected_script_new(filename, reflection_context);
    GjsCoverageFileStatistics *stats =
        create_statistics_from_reflection(reflected_script);
    g_object_unref(reflected_script);

    return stats;
}

static void
gjs_coverage_new_script_available_hook(GjsDebugHooks      *reg,
                                       GjsContext         *context,
                                       GjsDebugScriptInfo *info,
                                       gpointer            user_data)
{
    const gchar *filename = gjs_debug_script_info_get_filename(info);
    GjsCoverage *coverage = (GjsCoverage *) user_data;
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    GHashTable  *file_statistics = priv->file_statistics;

    if (g_hash_table_contains(file_statistics, filename)) {
        GjsCoverageFileStatistics *statistics =
            (GjsCoverageFileStatistics *) g_hash_table_lookup(file_statistics,
                                                              filename);

        if (!statistics) {
            statistics = new_statistics_for_filename(priv->reflection_context, filename);

            /* If create_statistics_for_filename returns NULL then we can
             * just bail out here, the stats print function will handle
             * the NULL case */
            if (!statistics)
                return;

            g_hash_table_insert(file_statistics,
                                g_strdup(filename),
                                statistics);
        }
    }
}

static void
write_string_into_stream(GOutputStream *stream,
                         const gchar   *string)
{
    g_output_stream_write(stream, (gconstpointer) string, strlen(string) * sizeof(gchar), NULL, NULL);
}

static void
write_source_file_header(GOutputStream *stream,
                         const gchar   *source_file_path)
{
    write_string_into_stream(stream, "SF:");
    write_string_into_stream(stream, source_file_path);
    write_string_into_stream(stream, "\n");
}

typedef struct _FunctionHitCountData {
    GOutputStream *stream;
    unsigned int  *n_functions_found;
    unsigned int  *n_functions_hit;
} FunctionHitCountData;

static void
write_function_hit_count(GOutputStream *stream,
                         const char    *function_name,
                         unsigned int   hit_count,
                         unsigned int  *n_functions_found,
                         unsigned int  *n_functions_hit)
{
    char *line = g_strdup_printf("FNDA:%i,%s\n",
                                 hit_count,
                                 function_name);

    (*n_functions_found)++;

    if (hit_count > 0)
        (*n_functions_hit)++;

    write_string_into_stream(stream, line);
    g_free(line);
}

static void
write_functions_hit_counts(GOutputStream *stream,
                           GHashTable    *functions,
                           unsigned int  *n_functions_found,
                           unsigned int  *n_functions_hit)
{
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, functions);

    gpointer key, value;
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char    *function_key = (const char *) key;
        unsigned int  hit_count = GPOINTER_TO_INT(value);

        write_function_hit_count(stream,
                                 function_key,
                                 hit_count,
                                 n_functions_found,
                                 n_functions_hit);
    }
}

static void
write_function_foreach_func(gpointer key,
                            gpointer value,
                            gpointer user_data)
{
    GOutputStream *stream = (GOutputStream *) user_data;
    const char    *function_key = (const char *) key;

    write_string_into_stream(stream, "FN:");
    write_string_into_stream(stream, function_key);
    write_string_into_stream(stream, "\n");
}

static void
write_functions(GOutputStream *data_stream,
                GHashTable    *functions)
{
    g_hash_table_foreach(functions, write_function_foreach_func, data_stream);
}

static void
write_uint32_into_stream(GOutputStream *stream,
                         unsigned int   integer)
{
    char buf[32];
    g_snprintf(buf, 32, "%u", integer);
    g_output_stream_write(stream, (gconstpointer) buf, strlen(buf) * sizeof(char), NULL, NULL);
}

static void
write_int32_into_stream(GOutputStream *stream,
                        int            integer)
{
    char buf[32];
    g_snprintf(buf, 32, "%i", integer);
    g_output_stream_write(stream, (gconstpointer) buf, strlen(buf) * sizeof(char), NULL, NULL);
}

static void
write_function_coverage(GOutputStream *data_stream,
                        unsigned int  n_found_functions,
                        unsigned int  n_hit_functions)
{
    write_string_into_stream(data_stream, "FNF:");
    write_uint32_into_stream(data_stream, n_found_functions);
    write_string_into_stream(data_stream, "\n");

    write_string_into_stream(data_stream, "FNH:");
    write_uint32_into_stream(data_stream, n_hit_functions);
    write_string_into_stream(data_stream, "\n");
}

static void
for_each_element_in_array(GArray   *array,
                          GFunc     func,
                          gpointer  user_data)
{
    const gsize element_size = g_array_get_element_size(array);
    unsigned int i;
    char         *current_array_pointer = (char *) array->data;

    for (i = 0; i < array->len; ++i, current_array_pointer += element_size)
        (*func)(current_array_pointer, user_data);
}

typedef struct _WriteAlternativeData {
    unsigned int  *n_branch_alternatives_found;
    unsigned int  *n_branch_alternatives_hit;
    GOutputStream *output_stream;
    gpointer      *all_alternatives;
    gboolean      branch_point_was_hit;
} WriteAlternativeData;

typedef struct _WriteBranchInfoData {
    unsigned int *n_branch_alternatives_found;
    unsigned int *n_branch_alternatives_hit;
    GOutputStream *output_stream;
} WriteBranchInfoData;

static void
write_individual_branch(gpointer branch_ptr,
                        gpointer user_data)
{
    GjsCoverageBranchData *branch = (GjsCoverageBranchData *) branch_ptr;
    WriteBranchInfoData   *data = (WriteBranchInfoData *) user_data;

    /* This line is not a branch, don't write anything */
    if (!branch->branch_point)
        return;

    unsigned int i = 0;
    for (; i < branch->branch_alternatives_taken->len; ++i) {
        unsigned int alternative_counter = g_array_index(branch->branch_alternatives_taken,
                                                         unsigned int,
                                                         i);
        unsigned int branch_point = branch->branch_point;
        char         *hit_count_string = NULL;

        if (!branch->branch_hit)
            hit_count_string = g_strdup_printf("-");
        else
            hit_count_string = g_strdup_printf("%i", alternative_counter);

        char *branch_alternative_line = g_strdup_printf("BRDA:%i,0,%i,%s\n",
                                                        branch_point,
                                                        i,
                                                        hit_count_string);

        write_string_into_stream(data->output_stream, branch_alternative_line);
        g_free(hit_count_string);
        g_free(branch_alternative_line);

        ++(*data->n_branch_alternatives_found);

        if (alternative_counter > 0)
            ++(*data->n_branch_alternatives_hit);
    }
}

static void
write_branch_coverage(GOutputStream *stream,
                      GArray        *branches,
                      unsigned int  *n_branch_alternatives_found,
                      unsigned int  *n_branch_alternatives_hit)

{
    /* Write individual branches and pass-out the totals */
    WriteBranchInfoData data = {
        n_branch_alternatives_found,
        n_branch_alternatives_hit,
        stream
    };

    for_each_element_in_array(branches,
                              write_individual_branch,
                              &data);
}

static void
write_branch_totals(GOutputStream *stream,
                    unsigned int   n_branch_alternatives_found,
                    unsigned int   n_branch_alternatives_hit)
{
    write_string_into_stream(stream, "BRF:");
    write_uint32_into_stream(stream, n_branch_alternatives_found);
    write_string_into_stream(stream, "\n");

    write_string_into_stream(stream, "BRH:");
    write_uint32_into_stream(stream, n_branch_alternatives_hit);
    write_string_into_stream(stream, "\n");
}

static void
write_line_coverage(GOutputStream *stream,
                    GArray        *stats,
                    unsigned int  *lines_hit_count,
                    unsigned int  *executable_lines_count)
{
    unsigned int i = 0;
    for (i = 0; i < stats->len; ++i) {
        int hit_count_for_line = g_array_index(stats, int, i);

        if (hit_count_for_line == -1)
            continue;

        write_string_into_stream(stream, "DA:");
        write_uint32_into_stream(stream, i);
        write_string_into_stream(stream, ",");
        write_int32_into_stream(stream, hit_count_for_line);
        write_string_into_stream(stream, "\n");

        if (hit_count_for_line > 0)
          ++(*lines_hit_count);

        ++(*executable_lines_count);
    }
}

static void
write_line_totals(GOutputStream *stream,
                  unsigned int   lines_hit_count,
                  unsigned int   executable_lines_count)
{
    write_string_into_stream(stream, "LH:");
    write_uint32_into_stream(stream, lines_hit_count);
    write_string_into_stream(stream, "\n");

    write_string_into_stream(stream, "LF:");
    write_uint32_into_stream(stream, executable_lines_count);
    write_string_into_stream(stream, "\n");
}

static void
write_end_of_record(GOutputStream *stream)
{
    write_string_into_stream(stream, "end_of_record\n");
}

static void
copy_source_file_to_coverage_output(const char *source,
                                    const char *destination)
{
    GFile *source_file = g_file_new_for_commandline_arg(source);
    GFile *destination_file = g_file_new_for_commandline_arg(destination);
    GError *error = NULL;

    /* We also need to recursively make the directory we
     * want to copy to, as g_file_copy doesn't do that */
    gchar *destination_dirname = g_path_get_dirname(destination);
    g_mkdir_with_parents(destination_dirname, S_IRWXU);

    if (!g_file_copy(source_file,
                     destination_file,
                     G_FILE_COPY_OVERWRITE,
                     NULL,
                     NULL,
                     NULL,
                     &error)) {
        g_critical("Failed to copy source file %s to destination %s: %s\n",
                   source,
                   destination,
                   error->message);
    }

    g_clear_error(&error);

    g_free(destination_dirname);
    g_object_unref(destination_file);
    g_object_unref(source_file);
}

typedef struct _StatisticsPrintUserData {
    GjsContext        *reflection_context;
    GFileOutputStream *ostream;
    const gchar       *output_directory;
} StatisticsPrintUserData;

/* This function will strip a URI scheme and return
 * the string with the URI scheme stripped or NULL
 * if the path was not a valid URI
 */
static const char *
strip_uri_scheme(const char *potential_uri)
{
    char *uri_header = g_uri_parse_scheme(potential_uri);

    if (uri_header) {
        gsize offset = strlen(uri_header);
        g_free(uri_header);

        /* g_uri_parse_scheme only parses the name
         * of the scheme, we also need to strip the
         * characters '://' */
        return potential_uri + offset + 3;
    }

    return NULL;
}

/* This function will return a string of pathname
 * components from the first directory indicating
 * where two directories diverge. For instance:
 *
 * child_path: /a/b/c/d/e
 * parent_path: /a/b/d/
 *
 * Will return: c/d/e
 *
 * If the directories are not at all similar then
 * the full dirname of the child_path effectively
 * be returned.
 *
 * As a special case, child paths that are a URI
 * automatically return the full URI path with
 * the URI scheme stripped out.
 */
static char *
find_diverging_child_components(const char *child_path,
                                const char *parent_path)
{
    const char *stripped_uri = strip_uri_scheme(child_path);

    if (stripped_uri)
        return g_strdup(stripped_uri);

    char **child_path_components = g_strsplit(child_path, "/", -1);
    char **parent_path_components = g_strsplit(parent_path, "/", -1);
    char **child_path_component_iterator = child_path_components;
    char **parent_path_component_iterator = parent_path_components;

    for (; *child_path_component_iterator != NULL &&
           *parent_path_component_iterator != NULL;
           ++child_path_component_iterator,
           ++parent_path_component_iterator) {
        if (g_strcmp0(*child_path_component_iterator,
                      *parent_path_component_iterator))
            break;
    }

    /* Paste the child path components back together */
    char *diverged = g_strjoinv("/", child_path_component_iterator);

    g_strfreev(child_path_components);
    g_strfreev(parent_path_components);

    return diverged;
}

/* The coverage output directory could be a relative path
 * so we need to get an absolute path */
static char *
get_absolute_path(const char *path)
{
    char *absolute_path = NULL;

    if (!g_path_is_absolute(path)) {
        char *current_dir = g_get_current_dir();
        absolute_path = g_build_filename(current_dir,
                                                path,
                                                NULL);
        g_free(current_dir);
    } else {
        absolute_path = g_strdup(path);
    }

    return absolute_path;
}

static void
print_statistics_for_files(gpointer key,
                           gpointer value,
                           gpointer user_data)
{
    StatisticsPrintUserData   *statistics_print_data = (StatisticsPrintUserData *) user_data;
    const char                *filename = (const char *) key;
    GjsCoverageFileStatistics *stats = (GjsCoverageFileStatistics *) value;

    /* If there is no statistics for this file, then we should
     * compile the script and print statistics for it now */
    if (!stats)
        stats = new_statistics_for_filename(statistics_print_data->reflection_context,
                                                             filename);

    /* Still couldn't create statistics, bail out */
    if (!stats)
        return;

    /* get_appropriate_tracefile_ref will automatically set the write
     * pointer to the correct place in the file */
    GOutputStream *ostream = G_OUTPUT_STREAM(statistics_print_data->ostream);

    char *absolute_output_directory = get_absolute_path(statistics_print_data->output_directory);
    char *diverged_paths =
        find_diverging_child_components(filename,
                                        absolute_output_directory);
    char *destination_filename = g_build_filename(absolute_output_directory,
                                                  diverged_paths,
                                                  NULL);

    copy_source_file_to_coverage_output(filename, destination_filename);

    write_source_file_header(ostream, (const char *) destination_filename);
    write_functions(ostream,
                    stats->functions);

    unsigned int functions_hit_count = 0;
    unsigned int functions_found_count = 0;

    write_functions_hit_counts(ostream,
                               stats->functions,
                               &functions_found_count,
                               &functions_hit_count);
    write_function_coverage(ostream,
                            functions_found_count,
                            functions_hit_count);

    unsigned int branches_hit_count = 0;
    unsigned int branches_found_count = 0;

    write_branch_coverage(ostream,
                          stats->branches,
                          &branches_found_count,
                          &branches_hit_count);
    write_branch_totals(ostream,
                        branches_found_count,
                        branches_hit_count);

    unsigned int lines_hit_count = 0;
    unsigned int executable_lines_count = 0;

    write_line_coverage(ostream,
                        stats->lines,
                        &lines_hit_count,
                        &executable_lines_count);
    write_line_totals(ostream,
                      lines_hit_count,
                      executable_lines_count);
    write_end_of_record(ostream);

    /* If value was initially NULL, then we should unref stats here */
    if (!value)
        gjs_coverage_file_statistics_destroy(stats);

    g_free(diverged_paths);
    g_free(destination_filename);
    g_free(absolute_output_directory);
}

void
gjs_coverage_write_statistics(GjsCoverage *coverage,
                              const char  *output_directory)
{
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    GError *error = NULL;
    GFileOutputStream *ostream = NULL;

    /* Create output_directory if it doesn't exist */
    g_mkdir_with_parents(output_directory, 0755);

    char  *output_file_path = g_build_filename(output_directory,
                                               "coverage.lcov",
                                               NULL);
    GFile *output_file = g_file_new_for_commandline_arg(output_file_path);
    g_free (output_file_path);

    /* Remove our new script hook so that we don't get spurious calls
     * to it whilst compiling new scripts */
    gjs_debug_hooks_remove_script_load_hook(priv->debug_hooks, priv->new_scripts_connection);
    priv->new_scripts_connection = 0;

    ostream = g_file_append_to(output_file,
                               G_FILE_CREATE_NONE,
                               NULL,
                               &error);

    if (!ostream) {
        char *output_file_path = g_file_get_path(output_file);
        g_warning("Unable to open output file %s: %s",
                  output_file_path,
                  error->message);
        g_free(output_file_path);
        g_error_free(error);
    }

    /* print_statistics_for_files can handle the NULL
     * case just fine, so there's no need to return if
     * output_file is NULL */
    StatisticsPrintUserData data = {
        priv->reflection_context,
        ostream,
        output_directory
    };

    g_hash_table_foreach(priv->file_statistics,
                         print_statistics_for_files,
                         &data);

    g_object_unref(ostream);
    g_object_unref(output_file);

    /* Re-insert our new script hook in case we need it again */
    priv->new_scripts_connection =
        gjs_debug_hooks_add_script_load_hook(priv->debug_hooks,
                                             gjs_coverage_new_script_available_hook,
                                             coverage);
}

static void
destroy_coverage_statistics_if_if_nonnull(gpointer statistics)
{
    if (statistics)
        gjs_coverage_file_statistics_destroy(statistics);
}

static void
gjs_coverage_init(GjsCoverage *self)
{
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(self);

    priv->reflection_context =  gjs_reflected_script_create_reflection_context();
    priv->file_statistics = g_hash_table_new_full(g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  destroy_coverage_statistics_if_if_nonnull);
    priv->active_branch = NULL;
}

static void
gjs_coverage_constructed(GObject *object)
{
    G_OBJECT_CLASS(gjs_coverage_parent_class)->constructed(object);

    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    /* Take the list of covered paths and add them to the coverage report */
    if (priv->covered_paths) {
        const char **iterator = (const char **) priv->covered_paths;

        do {
            /* At the moment we just  a key with no value to the
             * filename statistics. We'll create a proper source file
             * map once we get a new script callback (to avoid lots
             * of recompiling) and also create a source map on
             * coverage data generation if we didn't already have one */
            g_hash_table_insert(priv->file_statistics,
                                g_strdup((*iterator)),
                                NULL);
        } while (*(++iterator));
    }

    /* Add hook for new scripts and singlestep execution */
    priv->new_scripts_connection =
        gjs_debug_hooks_add_script_load_hook(priv->debug_hooks,
                                             gjs_coverage_new_script_available_hook,
                                             coverage);

    priv->single_step_connection =
        gjs_debug_hooks_add_singlestep_hook(priv->debug_hooks,
                                            gjs_coverage_single_step_interrupt_hook,
                                            coverage);

    priv->frame_step_connection =
        gjs_debug_hooks_add_frame_step_hook(priv->debug_hooks,
                                            gjs_coverage_frame_execution_hook,
                                            priv->file_statistics);
}

static void
gjs_coverage_set_property(GObject      *object,
                          unsigned int  prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);
    switch (prop_id) {
    case PROP_DEBUG_HOOKS:
        priv->debug_hooks = GJS_DEBUG_HOOKS(g_value_dup_object(value));
        break;
    case PROP_COVERAGE_PATHS:
        g_assert(priv->covered_paths == NULL);
        priv->covered_paths = (char **) g_value_dup_boxed (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

typedef void (*HookRemovalFunc) (GjsDebugHooks *, guint);

static void
clear_debug_handle(GjsDebugHooks   *hooks,
                   HookRemovalFunc  remove,
                   unsigned int    *handle)
{
    if (*handle) {
        remove(hooks, *handle);
        *handle = 0;
    }
}

static void
gjs_coverage_dispose(GObject *object)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE (object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    clear_debug_handle(priv->debug_hooks, gjs_debug_hooks_remove_script_load_hook, &priv->new_scripts_connection);
    clear_debug_handle(priv->debug_hooks, gjs_debug_hooks_remove_singlestep_hook, &priv->single_step_connection);
    clear_debug_handle(priv->debug_hooks, gjs_debug_hooks_remove_frame_step_hook, &priv->frame_step_connection);

    g_clear_object(&priv->debug_hooks);
    g_clear_object(&priv->reflection_context);

    G_OBJECT_CLASS(gjs_coverage_parent_class)->dispose(object);
}

static void
gjs_coverage_finalize (GObject *object)
{
    GjsCoverage *coverage = GJS_DEBUG_COVERAGE(object);
    GjsCoveragePrivate *priv = (GjsCoveragePrivate *) gjs_coverage_get_instance_private(coverage);

    g_hash_table_unref(priv->file_statistics);
    g_strfreev(priv->covered_paths);

    G_OBJECT_CLASS(gjs_coverage_parent_class)->finalize(object);
}

static void
gjs_coverage_class_init (GjsCoverageClass *klass)
{
    GObjectClass *object_class = (GObjectClass *) klass;

    object_class->constructed = gjs_coverage_constructed;
    object_class->dispose = gjs_coverage_dispose;
    object_class->finalize = gjs_coverage_finalize;
    object_class->set_property = gjs_coverage_set_property;

    properties[PROP_DEBUG_HOOKS] = g_param_spec_object("debug-hooks",
                                                       "Debug Hooks",
                                                       "Debug Hooks",
                                                       GJS_TYPE_DEBUG_HOOKS,
                                                       (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));
    properties[PROP_COVERAGE_PATHS] = g_param_spec_boxed("coverage-paths",
                                                         "Coverage Paths",
                                                         "Paths (and included subdirectories) of which to perform coverage analysis",
                                                         G_TYPE_STRV,
                                                         (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    g_object_class_install_properties(object_class,
                                      PROP_N,
                                      properties);
}

/**
 * gjs_coverage_new:
 * @debug_hooks: (transfer full): A #GjsDebugHooks to register callbacks on.
 * @coverage_paths: (transfer none): A null-terminated strv of directories to generate
 * coverage_data for
 *
 * Returns: A #GjsDebugCoverage
 */
GjsCoverage *
gjs_coverage_new (GjsDebugHooks *debug_hooks,
                  const char    **coverage_paths)
{
    GjsCoverage *coverage =
        GJS_DEBUG_COVERAGE(g_object_new(GJS_TYPE_DEBUG_COVERAGE,
                                        "debug-hooks", debug_hooks,
                                        "coverage-paths", coverage_paths,
                                        NULL));

    return coverage;
}
