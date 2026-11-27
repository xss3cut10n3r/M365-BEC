#include "app.h"

#include <stdio.h>

#include "../io/io.h"

#ifdef G_OS_WIN32
#include <windows.h>
#endif

// GUI builds lack consoles.
void cli_console(void) {
#ifdef G_OS_WIN32
    // Keep redirected output streams.
    gboolean out = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
    gboolean err = GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN;
    if ((!out || !err) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!out)
            freopen("CONOUT$", "w", stdout);
        if (!err)
            freopen("CONOUT$", "w", stderr);
    }
#endif
}

static void progress(const char *message, gpointer user) {
    (void)user;
    fprintf(stderr, "%s\n", message);
}

// Applies command-line verdicts.
static int cli_mark(sqlite3 *db, const Options *options) {
    static const char *const words[][2] = {{"good", "Good"}, {"bad", "Bad"}, {"unsure", "Unsure"}, {"clear", "Unmarked"}};
    const char *verdict = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(words); i++)
        if (!g_ascii_strcasecmp(options->mark, words[i][0]))
            verdict = words[i][1];
    gboolean ips = options->mark_ips && options->mark_ips[0];
    if (!verdict || (!ips && !(options->filter && *options->filter))) {
        fprintf(stderr, "Use --mark good|bad|unsure|clear with --filter BQL or --mark-ip IP.\n");
        return 2;
    }
    GError *error = NULL;
    if (ips) {
        GPtrArray *pairs = g_ptr_array_new_with_free_func(g_free);
        for (char **ip = options->mark_ips; *ip && !error; ip++) {
            char *address = ip_normalize(*ip);
            if (!*address)
                bec_fail(&error, "Invalid IP address: %s", *ip);
            g_ptr_array_add(pairs, address);
            g_ptr_array_add(pairs, g_strdup(""));
        }
        if (!error && assign_verdict(db, "ip", pairs, verdict, &error))
            printf("Marked %u IP addresses %s\n", pairs->len / 2, verdict);
        g_ptr_array_free(pairs, TRUE);
    } else {
        gint64 count = mark_matches(db, options->log, options->filter, verdict, 0, &error);
        if (!error)
            printf("Marked %" G_GINT64_FORMAT " events %s\n", count, verdict);
    }
    if (error) {
        fprintf(stderr, "%s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    return 0;
}

// Import and export without windows.
int cli_run(const Options *options) {
    if (!options->case_path) {
        fprintf(stderr, "Use --case CASE.bec with --export, --mark or --no-gui.\n");
        return 2;
    }
    // Exports never replace inputs.
    for (char **file = options->files; options->export_path && file && *file; file++) {
        if (same_file(options->export_path, *file)) {
            fprintf(stderr, "Export failed: %s is an input file. Choose another destination.\n", options->export_path);
            return 2;
        }
    }
    GError *error = NULL;
    sqlite3 *db = case_connect(options->case_path, &error);
    if (!db) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    int status = 0;
    for (char **file = options->files; file && *file; file++) {
        char *name = g_path_get_basename(*file);
        ImportState state = import_file(db, *file, progress, NULL, &error);
        if (state == IMPORT_DONE) {
            printf("Imported %s\n", name);
        } else if (state == IMPORT_DUPLICATE) {
            printf("Already imported %s\n", name);
        } else {
            fprintf(stderr, "Not imported %s: %s\n", name, error ? error->message : "failed");
            status = 1;
        }
        g_clear_error(&error);
        g_free(name);
    }
    sqlite3_stmt *warnings = sql_prepare(db, "SELECT name,warning FROM logs WHERE warning!='' ORDER BY id", NULL);
    while (warnings && sqlite3_step(warnings) == SQLITE_ROW)
        fprintf(stderr, "Warning %s: %s\n", column_text(warnings, 0), column_text(warnings, 1));
    sqlite3_finalize(warnings);
    const char *filter = options->filter ? options->filter : "";
    if (options->mark) {
        int marked = cli_mark(db, options);
        if (marked || !options->export_path) {
            sqlite3_close(db);
            fflush(stdout);
            return status ? status : marked;
        }
        // IP marks export everything.
        if (options->mark_ips && options->mark_ips[0] && !options->filter)
            filter = "marked = true";
    }
    if (options->export_path) {
        if (export_events(db, options->export_path, options->log, filter, FALSE, "time", NULL, &error)) {
            gint64 count = count_rows(db, options->log, filter, NULL);
            printf("Exported %" G_GINT64_FORMAT " events to %s\n", count, options->export_path);
        } else {
            fprintf(stderr, "Export failed: %s\n", error->message);
            g_clear_error(&error);
            status = 1;
        }
    } else {
        gint64 count = count_rows(db, options->log, filter, &error);
        if (error) {
            fprintf(stderr, "%s\n", error->message);
            g_clear_error(&error);
            status = 1;
        } else {
            printf("%" G_GINT64_FORMAT " events match\n", count);
        }
    }
    sqlite3_close(db);
    fflush(stdout);
    return status;
}
