#include "../src/io/io.h"

#include <stdio.h>
#include <string.h>

// Prints one JSON array.
static void emit(GPtrArray *cells) {
    GString *out = g_string_new("[");
    for (guint i = 0; i < cells->len; i++) {
        if (i)
            g_string_append_c(out, ',');
        if (cells->pdata[i])
            j_quote(out, cells->pdata[i], -1);
        else
            g_string_append(out, "null");
    }
    g_string_append(out, "]\n");
    fwrite(out->str, 1, out->len, stdout);
    g_string_free(out, TRUE);
    g_ptr_array_set_size(cells, 0);
}

static char *argument(const char *value) {
    char *text = NULL;
    if (value[0] == '@' && g_file_get_contents(value + 1, &text, NULL, NULL))
        return text;
    return g_strdup(value);
}

static GPtrArray *custom_columns(int argc, char **argv, int start) {
    GPtrArray *custom = g_ptr_array_new();
    for (int i = start; i < argc; i++) {
        CustomColumn *column = g_new0(CustomColumn, 1);
        column->label = column->path = argument(argv[i]);
        g_ptr_array_add(custom, column);
    }
    return custom;
}

static int fail(GError *error) {
    printf("ERROR\t%s\n", error ? error->message : "unknown");
    return 1;
}

static int lines(const char *mode) {
    char *line = NULL;
    gsize length = 0;
    GIOChannel *input = g_io_channel_unix_new(0);
    g_io_channel_set_encoding(input, NULL, NULL);
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    while (g_io_channel_read_line(input, &line, &length, NULL, NULL) == G_IO_STATUS_NORMAL) {
        JDoc *doc = j_parse(line, -1, 0, NULL);
        const char *value = doc && doc->root->t == J_STR ? doc->root->u.s : "";
        if (!strcmp(mode, "utc")) {
            g_ptr_array_add(cells, utc_normalize(value));
        } else if (!strcmp(mode, "ip")) {
            g_ptr_array_add(cells, ip_normalize(value));
        } else if (!strcmp(mode, "header")) {
            g_ptr_array_add(cells, clean_header(value));
        } else {
            GError *error = NULL;
            Query *query = query_new(value, &error);
            g_ptr_array_add(cells, g_strdup(query ? "ok" : "error"));
            if (query) {
                for (guint i = 0; i < query->args->len; i++) {
                    Arg *arg = query->args->pdata[i];
                    g_ptr_array_add(cells, arg->integer ? g_strdup_printf("%" G_GINT64_FORMAT, arg->number) : g_strdup(arg->text));
                }
            } else {
                g_ptr_array_add(cells, g_strdup(error->message));
            }
            g_clear_error(&error);
            query_free(query);
        }
        emit(cells);
        j_free(doc);
        g_free(line);
    }
    return 0;
}

static void progress(const char *message, gpointer user) {
    (void)user;
    fprintf(stderr, "%s\n", message);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    const char *command = argv[1];
    if (argc == 2)
        return lines(command);
    GError *error = NULL;
    sqlite3 *db = case_connect(argv[2], &error);
    if (!db)
        return fail(error);
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    int status = 0;
    if (!strcmp(command, "import")) {
        for (int i = 3; i < argc; i++) {
            ImportState state = import_file(db, argv[i], getenv("BEC_PROGRESS") ? progress : NULL, NULL, &error);
            g_ptr_array_add(cells, g_path_get_basename(argv[i]));
            g_ptr_array_add(cells, g_strdup(state == IMPORT_DONE ? "Imported" : state == IMPORT_DUPLICATE ? "Already imported" : "Failed"));
            g_ptr_array_add(cells, g_strdup(error ? error->message : ""));
            g_clear_error(&error);
            emit(cells);
        }
    } else if (!strcmp(command, "rows") && argc >= 7) {
        char *filter = argument(argv[4]);
        GPtrArray *custom = custom_columns(argc, argv, 7);
        sqlite3_stmt *rows = query_rows(db, atoll(argv[3]), filter, atoi(argv[6]), argv[5], custom, &error);
        if (!rows)
            return fail(error);
        while (sqlite3_step(rows) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(rows); i++)
                g_ptr_array_add(cells, g_strdup(column_text(rows, i)));
            emit(cells);
        }
        sqlite3_finalize(rows);
    } else if (!strcmp(command, "count") && argc >= 5) {
        char *filter = argument(argv[4]);
        gint64 count = count_rows(db, atoll(argv[3]), filter, &error);
        if (error)
            return fail(error);
        printf("%" G_GINT64_FORMAT "\n", count);
    } else if ((!strcmp(command, "stats") || !strcmp(command, "drill")) && argc >= 7) {
        char *filter = argument(argv[5]);
        gint64 log = atoll(argv[4]);
        int interval = atoi(argv[6]);
        GPtrArray *rows = stats_calculate(db, argv[3], log, filter, interval, &error);
        if (!rows)
            return fail(error);
        const Report *report = report_find(argv[3]);
        gboolean activity = !strcmp(argv[3], "activity");
        for (guint i = 0; i < rows->len; i++) {
            StatRow *row = rows->pdata[i];
            if (!strcmp(command, "drill")) {
                char *drill = stats_drilldown(argv[3], row, interval);
                char *combined = combine_filters(filter, drill);
                gint64 count = count_rows(db, log, combined, &error);
                if (count != row->count) {
                    printf("MISMATCH\t%s\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\n", combined, count, row->count);
                    status = 1;
                }
                g_clear_error(&error);
                g_free(drill);
                g_free(combined);
                continue;
            }
            for (int k = 0; k < report_width(report); k++)
                g_ptr_array_add(cells, activity ? g_strdup_printf("%" G_GINT64_FORMAT, row->epoch) : g_strdup(row->values[k]));
            g_ptr_array_add(cells, g_strdup_printf("%" G_GINT64_FORMAT, row->count));
            g_ptr_array_add(cells, g_strdup(row->first));
            g_ptr_array_add(cells, g_strdup(row->last));
            emit(cells);
        }
        if (!strcmp(command, "drill"))
            printf("checked\t%u\n", rows->len);
    } else if (!strcmp(command, "export") && argc >= 8) {
        char *filter = argument(argv[5]);
        GPtrArray *custom = custom_columns(argc, argv, 8);
        if (!export_events(db, argv[3], atoll(argv[4]), filter, atoi(argv[7]), argv[6], custom, &error))
            return fail(error);
        printf("ok\n");
    } else if (!strcmp(command, "extract") && argc >= 5) {
        if (!extract_evidence(db, atoll(argv[3]), argv[4], &error))
            return fail(error);
        printf("ok\n");
    } else if (!strcmp(command, "verdict") && argc >= 5) {
        GPtrArray *pairs = g_ptr_array_new();
        for (int i = 5; i < argc; i++)
            g_ptr_array_add(pairs, argv[i]);
        if (!assign_verdict(db, argv[3], pairs, argv[4], &error))
            return fail(error);
        printf("ok\n");
    } else if (!strcmp(command, "undo")) {
        if (!undo_verdict(db, &error))
            return fail(error);
        printf("ok\n");
    } else if (!strcmp(command, "quality")) {
        GPtrArray *rows = stats_quality(db, &error);
        for (guint i = 0; rows && i < rows->len; i++) {
            QualityRow *row = rows->pdata[i];
            g_ptr_array_add(cells, g_strdup(row->name));
            g_ptr_array_add(cells, g_strdup(row->kind));
            g_ptr_array_add(cells, g_strdup_printf("%" G_GINT64_FORMAT, row->total));
            g_ptr_array_add(cells, g_strdup_printf("%" G_GINT64_FORMAT, row->issues));
            g_ptr_array_add(cells, row->linked < 0 ? NULL : g_strdup_printf("%" G_GINT64_FORMAT, row->linked));
            g_ptr_array_add(cells, row->missing < 0 ? NULL : g_strdup_printf("%" G_GINT64_FORMAT, row->missing));
            g_ptr_array_add(cells, g_strdup(row->warning));
            emit(cells);
        }
    } else {
        status = 2;
    }
    sqlite3_close(db);
    return status;
}
