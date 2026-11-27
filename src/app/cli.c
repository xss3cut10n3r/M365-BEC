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

// Import and export without windows.
int cli_run(const Options *options) {
    if (!options->case_path) {
        fprintf(stderr, "Use --case CASE.bec with --export or --no-gui.\n");
        return 2;
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
