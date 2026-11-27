#include "app.h"

#include "../ui/ui.h"

static Options options;

static void activate(GtkApplication *gtk, gpointer user) {
    (void)user;
    style_install();
    // Bundled icons sit beside binaries.
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *folder = g_path_get_dirname(self);
        char *icons = g_build_filename(folder, "..", "share", "icons", NULL);
        gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), icons);
        g_free(icons);
        g_free(folder);
        g_free(self);
    }
    App *app = app_new(gtk);
    gtk_window_present(app->window);
    if (options.case_path)
        app_load(app, options.case_path, !g_file_test(options.case_path, G_FILE_TEST_EXISTS));
    if (options.filter && app->db)
        app_apply(app, options.filter);
    if (options.files && options.files[0]) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        for (char **file = options.files; *file; file++)
            g_ptr_array_add(paths, g_strdup(*file));
        app_open_files_then(app, paths);
    }
}

int main(int argc, char **argv) {
    GOptionEntry entries[] = {
        {"case", 0, 0, G_OPTION_ARG_FILENAME, &options.case_path, "Case file to open or create", "CASE.bec"},
        {"filter", 0, 0, G_OPTION_ARG_STRING, &options.filter, "Initial BQL display filter", "BQL"},
        {"export", 0, 0, G_OPTION_ARG_FILENAME, &options.export_path, "Export matches as CSV, XLSX or JSON, then exit",
         "FILE"},
        {"log", 0, 0, G_OPTION_ARG_INT64, &options.log, "Limit --export to one log ID", "ID"},
        {"mark", 0, 0, G_OPTION_ARG_STRING, &options.mark, "Mark --filter matches or --mark-ip addresses, then exit",
         "good|bad|unsure|clear"},
        {"mark-ip", 0, 0, G_OPTION_ARG_STRING_ARRAY, &options.mark_ips, "Mark every event from this IP (repeatable)", "IP"},
        {"no-gui", 0, 0, G_OPTION_ARG_NONE, &options.headless, "Import files into the case, then exit", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &options.files, NULL, "[CSV, XLSX or JSON logs...]"},
        {NULL},
    };
    GOptionContext *context = g_option_context_new(NULL);
    g_option_context_set_summary(context, APP_TITLE);
    g_option_context_add_main_entries(context, entries, NULL);
    GError *error = NULL;
#ifdef G_OS_WIN32
    char **arguments = g_win32_get_command_line();
#else
    char **arguments = g_strdupv(argv);
#endif
    (void)argc;
    gboolean wants_help = FALSE;
    for (char **argument = arguments + 1; *argument; argument++)
        wants_help = wants_help || !g_strcmp0(*argument, "--help") || !g_strcmp0(*argument, "-h");
    if (wants_help)
        cli_console();
    if (!g_option_context_parse_strv(context, &arguments, &error)) {
        cli_console();
        g_printerr("%s\n", error->message);
        return 2;
    }
    g_option_context_free(context);
    if (options.export_path || options.headless || options.mark) {
        cli_console();
        return cli_run(&options);
    }
#ifdef G_OS_WIN32
    // Use the native title bar.
    g_setenv("GTK_CSD", "0", FALSE);
    // Software drawing starts faster.
    g_setenv("GSK_RENDERER", "cairo", FALSE);
#endif
    GtkApplication *gtk = gtk_application_new("io.github.xss3cut10n3r.M365BEC", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(gtk, "activate", G_CALLBACK(activate), NULL);
    char *program[] = {argv[0], NULL};
    int status = g_application_run(G_APPLICATION(gtk), 1, program);
    g_object_unref(gtk);
    return status;
}
