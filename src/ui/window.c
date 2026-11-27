#include "ui.h"

#include <string.h>

#define EVENT_JOIN " FROM events e JOIN logs l ON l.id=e.log"

static void update_actions(App *app);
static void go_home(App *app);
static void refresh_logs(App *app);
static void update_case_bar(App *app);

/* Settings. */

gboolean setting_bool(App *app, const char *key, gboolean fallback) {
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(app->settings, "window", key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }
    return value;
}

void setting_set_bool(App *app, const char *key, gboolean value) {
    g_key_file_set_boolean(app->settings, "window", key, value);
}

static void settings_save(App *app) {
    char *folder = g_path_get_dirname(app->settings_path);
    g_mkdir_with_parents(folder, 0700);
    g_key_file_save_to_file(app->settings, app->settings_path, NULL);
    g_free(folder);
}

static char **recent_paths(App *app) {
    char **paths = g_key_file_get_string_list(app->settings, "cases", "recentCases", NULL, NULL);
    return paths ? paths : g_new0(char *, 1);
}

/* Small helpers. */

void app_message(App *app, const char *text) {
    gtk_label_set_text(GTK_LABEL(app->status), text);
}

void app_alert(App *app, const char *title, const char *detail) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_show(dialog, app->window);
    g_object_unref(dialog);
}

gboolean app_busy(App *app) {
    if (app->busy)
        app_alert(app, APP_TITLE, "A file operation is still running.");
    return app->busy;
}

void add_text_action(GMenu *menu, const char *label, const char *action, const char *target) {
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(target));
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

LogInfo *app_log(App *app, gint64 id) {
    for (guint i = 0; i < app->logs->len; i++)
        if (((LogInfo *)app->logs->pdata[i])->id == id)
            return app->logs->pdata[i];
    return NULL;
}

char *app_log_name(const LogInfo *log) {
    if (!log)
        return g_strdup("All case events");
    if (*log->sheet && !log_is_json(log->name))
        return g_strdup_printf("%s / %s", log->name, log->sheet);
    return g_strdup(log->name);
}

static void view_free(gpointer data) {
    View *view = data;
    g_free(view->query);
    g_free(view->draft);
    g_free(view->order);
    g_free(view);
}

static View *view_for(App *app, gint64 key) {
    View *view = g_hash_table_lookup(app->views, &key);
    if (!view) {
        view = g_new0(View, 1);
        view->query = g_strdup("");
        view->draft = g_strdup("");
        view->order = g_strdup("time");
        gint64 *stored = g_new(gint64, 1);
        *stored = key;
        g_hash_table_insert(app->views, stored, view);
    }
    return view;
}

View *app_view(App *app) {
    return view_for(app, app->active);
}

static void log_free(gpointer data) {
    LogInfo *log = data;
    g_free(log->name);
    g_free(log->sheet);
    g_free(log->kind);
    g_free(log->warning);
    g_free(log);
}

void app_saved(App *app, const char *what) {
    GDateTime *now = g_date_time_new_now_local();
    char *clock = g_date_time_format(now, "%H:%M:%S");
    char *text = g_strdup_printf("Saved %s \xc2\xb7 %s", clock, what);
    gtk_label_set_text(GTK_LABEL(app->case_saved), text);
    g_free(text);
    g_free(clock);
    g_date_time_unref(now);
}

/* Tabs. */

static gint64 page_key(GtkWidget *page) {
    return *(gint64 *)g_object_get_data(G_OBJECT(page), "key");
}

static int find_tab(App *app, gint64 key) {
    int count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->tabs));
    for (int i = 0; i < count; i++)
        if (page_key(gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->tabs), i)) == key)
            return i;
    return -1;
}

static void remember_view(App *app) {
    if (!app->has_active)
        return;
    View *view = app_view(app);
    g_free(view->draft);
    view->draft = g_strdup(gtk_editable_get_text(GTK_EDITABLE(app->entry)));
    int current = timeline_current(app->timeline);
    if (current >= 0) {
        view->selected = timeline_id(app->timeline, (guint)current);
        view->has_selected = TRUE;
        view->scroll = timeline_scroll(app->timeline);
    }
}

static void switched(App *app, int index);

static void close_index(App *app, int index) {
    if (index < 0 || index >= gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->tabs)))
        return;
    gint64 key = page_key(gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->tabs), index));
    if (app->has_active && key == app->active) {
        remember_view(app);
        app->has_active = FALSE;
    }
    app->switching = TRUE;
    gtk_notebook_remove_page(GTK_NOTEBOOK(app->tabs), index);
    app->switching = FALSE;
    g_array_append_val(app->closed_tabs, key);
    int current = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->tabs));
    if (current < 0)
        go_home(app);
    else if (!app->has_active || app->active != page_key(gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->tabs), current)))
        switched(app, current);
    else
        update_actions(app);
}

static void tab_close_clicked(GtkButton *button, gpointer user) {
    App *app = user;
    GtkWidget *page = g_object_get_data(G_OBJECT(button), "page");
    close_index(app, gtk_notebook_page_num(GTK_NOTEBOOK(app->tabs), page));
}

static int make_tab(App *app, gint64 key) {
    view_for(app, key);
    int existing = find_tab(app, key);
    if (existing >= 0)
        return existing;
    LogInfo *log = key ? app_log(app, key) : NULL;
    char *label = !key ? g_strdup("All case events")
                       : g_strdup(*log->sheet && !log_is_json(log->name) ? log->sheet : log->kind);
    int count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->tabs));
    for (int i = 0; key && i < count; i++) {
        GtkWidget *other = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->tabs), i);
        if (!g_strcmp0(g_object_get_data(G_OBJECT(other), "label"), label)) {
            char *numbered = g_strdup_printf("%s (%" G_GINT64_FORMAT ")", label, key);
            g_free(label);
            label = numbered;
            break;
        }
    }
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gint64 *stored = g_new(gint64, 1);
    *stored = key;
    g_object_set_data_full(G_OBJECT(page), "key", stored, g_free);
    g_object_set_data_full(G_OBJECT(page), "label", g_strdup(label), g_free);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *text = gtk_label_new(label);
    if (g_utf8_strlen(label, -1) > 30) {
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(GTK_LABEL(text), 30);
    }
    gtk_box_append(GTK_BOX(header), text);
    char *tip = g_strconcat("Close ", label, NULL);
    GtkWidget *close = close_button(tip);
    g_free(tip);
    g_object_set_data(G_OBJECT(close), "page", page);
    g_signal_connect(close, "clicked", G_CALLBACK(tab_close_clicked), app);
    gtk_box_append(GTK_BOX(header), close);
    char *full = key ? app_log_name(log) : g_strdup("All imported files");
    gtk_widget_set_tooltip_text(header, full);
    g_free(full);
    app->switching = TRUE;
    int index = gtk_notebook_append_page(GTK_NOTEBOOK(app->tabs), page, header);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app->tabs), page, TRUE);
    app->switching = FALSE;
    for (guint i = 0; i < app->closed_tabs->len; i++) {
        if (g_array_index(app->closed_tabs, gint64, i) == key) {
            g_array_remove_index(app->closed_tabs, i);
            break;
        }
    }
    g_free(label);
    return index;
}

static void switched(App *app, int index) {
    if (index < 0)
        return;
    remember_view(app);
    app->active = page_key(gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->tabs), index));
    app->has_active = TRUE;
    gtk_stack_set_visible_child_name(GTK_STACK(app->pages), "work");
    gtk_editable_set_text(GTK_EDITABLE(app->entry), app_view(app)->draft);
    update_actions(app);
    app_refresh(app);
}

void app_tab(App *app, gint64 key) {
    if (!app->db || (key && !app_log(app, key)))
        return;
    int index = make_tab(app, key);
    app->switching = TRUE;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->tabs), index);
    app->switching = FALSE;
    switched(app, index);
}

static void page_switched(GtkNotebook *notebook, GtkWidget *page, guint index, gpointer user) {
    (void)notebook;
    (void)page;
    App *app = user;
    if (!app->switching)
        switched(app, (int)index);
}

/* Queries run off-thread. */

typedef struct {
    App *app;
    guint token;
    GCancellable *cancel;
    char *path, *sql;
    GPtrArray *args;
    GArray *ids;
    char *error;
} QueryJob;

static int query_stop(void *cancel) {
    return g_cancellable_is_cancelled(cancel);
}

static void cancel_query(App *app) {
    if (app->query_cancel) {
        g_cancellable_cancel(app->query_cancel);
        g_clear_object(&app->query_cancel);
    }
    app->query_token++;
    app->loading = FALSE;
}

void app_inspect(App *app) {
    int current = timeline_current(app->timeline);
    Record *record = current >= 0 && app->db ? timeline_record(app->timeline, (guint)current) : NULL;
    if (record)
        inspector_set(app->inspector, app->db, record);
    else
        inspector_clear(app->inspector);
}

static GPtrArray *reload_logs(App *app);

// Other processes may add logs.
static void adopt_logs(App *app) {
    gint64 count = app->logs->len, total = 0;
    for (guint i = 0; i < app->logs->len; i++)
        total += ((LogInfo *)app->logs->pdata[i])->total;
    sqlite3_stmt *now = sql_prepare(app->db, "SELECT count(*),coalesce(sum(total),0) FROM logs", NULL);
    gboolean changed = now && sqlite3_step(now) == SQLITE_ROW &&
                       (sqlite3_column_int64(now, 0) != count || sqlite3_column_int64(now, 1) != total);
    sqlite3_finalize(now);
    if (!changed)
        return;
    g_ptr_array_free(reload_logs(app), TRUE);
    statistics_refresh_all(app);
}

static void show_rows(App *app, GArray *ids) {
    View *view = app_view(app);
    app->loading = FALSE;
    adopt_logs(app);
    guint count = ids->len;
    timeline_reset(app->timeline, app->db, ids);
    if (count) {
        int position = view->has_selected ? timeline_find(app->timeline, view->selected) : 0;
        timeline_select(app->timeline, position < 0 ? 0 : (guint)position);
        if (view->scroll > 0)
            timeline_set_scroll(app->timeline, view->scroll);
        app_inspect(app);
    }
    gint64 total = 0;
    guint warnings = 0;
    for (guint i = 0; i < app->logs->len; i++) {
        LogInfo *log = app->logs->pdata[i];
        if (app->active && log->id != app->active)
            continue;
        total += log->total;
        warnings += *log->warning != 0;
    }
    char *events = group_digits(total), *shown = group_digits(count);
    char *scope = g_strdup_printf("Events: %s    Displayed: %s", events, shown);
    gtk_label_set_text(GTK_LABEL(app->scope), scope);
    gtk_widget_set_tooltip_text(app->scope, app->path);
    char *limit = g_strdup_printf("\xe2\x9a\xa0  Export limits: %u", warnings);
    gtk_button_set_label(GTK_BUTTON(app->warnings), limit);
    gtk_widget_set_visible(app->warnings, warnings > 0);
    char *label = app_log_name(app->active ? app_log(app, app->active) : NULL);
    app_message(app, label);
    g_free(label);
    g_free(limit);
    g_free(scope);
    g_free(events);
    g_free(shown);
}

static gboolean query_done(gpointer data) {
    QueryJob *job = data;
    App *app = job->app;
    if (!g_cancellable_is_cancelled(job->cancel) && job->token == app->query_token && app->has_active) {
        if (job->error) {
            app->loading = FALSE;
            gtk_label_set_text(GTK_LABEL(app->scope), "Query failed");
            app_message(app, job->error);
        } else {
            show_rows(app, job->ids);
            job->ids = NULL;
        }
    }
    if (job->ids)
        g_array_free(job->ids, TRUE);
    g_object_unref(job->cancel);
    g_ptr_array_free(job->args, TRUE);
    g_free(job->path);
    g_free(job->sql);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer query_run(gpointer data) {
    QueryJob *job = data;
    GError *error = NULL;
    sqlite3 *db = case_read(job->path, &error);
    job->ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (db) {
        sqlite3_progress_handler(db, 1200, query_stop, job->cancel);
        sqlite3_stmt *statement = sql_prepare(db, job->sql, &error);
        if (statement) {
            args_bind(statement, job->args, 1);
            int state;
            while ((state = sqlite3_step(statement)) == SQLITE_ROW) {
                gint64 id = sqlite3_column_int64(statement, 0);
                g_array_append_val(job->ids, id);
            }
            if (state != SQLITE_DONE)
                bec_fail(&error, "%s", sqlite3_errmsg(db));
            sqlite3_finalize(statement);
        }
        sqlite3_close(db);
    }
    if (error) {
        job->error = g_strdup(error->message);
        g_clear_error(&error);
    }
    g_idle_add(query_done, job);
    return NULL;
}

void app_refresh(App *app) {
    if (!app->has_active || !app->db)
        return;
    cancel_query(app);
    app->query_cancel = g_cancellable_new();
    timeline_reset(app->timeline, app->db, g_array_new(FALSE, FALSE, sizeof(gint64)));
    inspector_clear(app->inspector);
    View *view = app_view(app);
    GPtrArray *args = args_new();
    GError *error = NULL;
    char *clause = where_sql(app->active, view->query, args, &error);
    char *order = clause ? sort_sql(view->order, view->desc, args, &error) : NULL;
    if (!order) {
        app_message(app, error ? error->message : "Query failed");
        g_clear_error(&error);
        g_free(clause);
        g_ptr_array_free(args, TRUE);
        return;
    }
    app->loading = TRUE;
    timeline_sort_indicator(app->timeline, view->order, view->desc);
    gtk_label_set_text(GTK_LABEL(app->scope), "Loading displayed events\xe2\x80\xa6");
    QueryJob *job = g_new0(QueryJob, 1);
    job->app = app;
    job->token = app->query_token;
    job->cancel = g_object_ref(app->query_cancel);
    job->path = g_strdup(app->path);
    job->sql = g_strconcat("SELECT e.id", EVENT_JOIN, clause, order, NULL);
    job->args = args;
    g_free(clause);
    g_free(order);
    g_thread_unref(g_thread_new("query", query_run, job));
}

void app_sort(App *app, const char *key, gboolean desc) {
    (void)desc;
    if (!app->has_active)
        return;
    remember_view(app);
    View *view = app_view(app);
    view->desc = !strcmp(view->order, key) ? !view->desc : FALSE;
    g_free(view->order);
    view->order = g_strdup(key);
    app_refresh(app);
}

/* Display filters. */

static void validate(GtkEditable *editable, gpointer user) {
    (void)user;
    const char *text = gtk_editable_get_text(editable);
    GError *error = NULL;
    Query *query = query_new(text, &error);
    char *stripped = py_strip(text);
    GtkWidget *entry = GTK_WIDGET(editable);
    gtk_widget_remove_css_class(entry, "valid");
    gtk_widget_remove_css_class(entry, "error");
    if (!query)
        gtk_widget_add_css_class(entry, "error");
    else if (*stripped)
        gtk_widget_add_css_class(entry, "valid");
    gtk_widget_set_tooltip_text(entry, query ? "Press Enter to apply" : error->message);
    g_clear_error(&error);
    query_free(query);
    g_free(stripped);
}

void app_apply(App *app, const char *value) {
    char *text = g_strdup(value ? value : gtk_editable_get_text(GTK_EDITABLE(app->entry)));
    gtk_editable_set_text(GTK_EDITABLE(app->entry), text);
    GError *error = NULL;
    Query *query = query_new(text, &error);
    if (!query) {
        app_message(app, error->message);
        g_clear_error(&error);
        g_free(text);
        return;
    }
    query_free(query);
    if (!app->db) {
        app_message(app, "Open a case or import logs to apply this filter.");
        g_free(text);
        return;
    }
    if (!app->has_active)
        app_tab(app, 0);
    gtk_editable_set_text(GTK_EDITABLE(app->entry), text);
    View *view = app_view(app);
    g_free(view->query);
    g_free(view->draft);
    view->query = py_strip(text);
    view->draft = g_strdup(view->query);
    view->has_selected = FALSE;
    view->scroll = 0;
    if (*text) {
        for (guint i = 0; i < app->history->len; i++) {
            if (!strcmp(app->history->pdata[i], text)) {
                g_ptr_array_remove_index(app->history, i);
                break;
            }
        }
        g_ptr_array_insert(app->history, 0, g_strdup(text));
        if (app->history->len > 10)
            g_ptr_array_set_size(app->history, 10);
    }
    g_free(text);
    app_refresh(app);
}

void app_apply_in(App *app, gint64 log, const char *text) {
    if (!app->has_active || log != app->active)
        app_tab(app, log);
    app_apply(app, text);
    gtk_window_present(app->window);
}

void app_prepare(App *app, const char *text) {
    gtk_editable_set_text(GTK_EDITABLE(app->entry), text);
    gtk_widget_grab_focus(app->entry);
}

void filter_menu_items(App *app, GMenu *menu, const char *expression) {
    const char *current = app->has_active ? app_view(app)->query : "";
    char *excluded = g_strdup_printf("NOT (%s)", expression);
    const char *names[5] = {"Selected", "Not selected"};
    char *queries[5] = {g_strdup(expression), g_strdup(excluded)};
    int count = 2;
    if (*current) {
        names[2] = "\xe2\x80\xa6 and selected";
        queries[2] = combine_filters(current, expression);
        names[3] = "\xe2\x80\xa6 or selected";
        queries[3] = g_strdup_printf("(%s) OR (%s)", current, expression);
        names[4] = "\xe2\x80\xa6 and not selected";
        queries[4] = combine_filters(current, excluded);
        count = 5;
    }
    const char *const titles[] = {"Apply as Filter", "Prepare a Filter"};
    const char *const actions[] = {"win.filter-apply", "win.filter-prepare"};
    for (int t = 0; t < 2; t++) {
        GMenu *sub = g_menu_new();
        for (int i = 0; i < count; i++)
            add_text_action(sub, names[i], actions[t], queries[i]);
        g_menu_append_submenu(menu, titles[t], G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
    add_text_action(menu, "Find matching events across case", "win.filter-case", expression);
    add_text_action(menu, "Copy BQL expression", "win.copy-text", expression);
    for (int i = 0; i < count; i++)
        g_free(queries[i]);
    g_free(excluded);
}

static void build_filter_menu(GtkMenuButton *button, gpointer user) {
    (void)button;
    App *app = user;
    g_menu_remove_all(app->filter_menu);
    GMenu *saved = g_menu_new(), *examples = g_menu_new(), *history = g_menu_new(), *help = g_menu_new();
    GPtrArray *filters = saved_filters(app);
    for (guint i = 0; i + 1 < filters->len; i += 2)
        add_text_action(saved, filters->pdata[i], "win.filter-apply", filters->pdata[i + 1]);
    g_ptr_array_free(filters, TRUE);
    for (int i = 0; i < EXAMPLE_COUNT; i++)
        add_text_action(examples, EXAMPLES[i][0], "win.filter-apply", EXAMPLES[i][1]);
    for (guint i = 0; i < app->history->len; i++)
        add_text_action(history, app->history->pdata[i], "win.filter-apply", app->history->pdata[i]);
    g_menu_append(help, "BQL reference\xe2\x80\xa6", "win.help");
    GMenu *sections[] = {saved, examples, history, help};
    for (int i = 0; i < 4; i++) {
        g_menu_append_section(app->filter_menu, NULL, G_MENU_MODEL(sections[i]));
        g_object_unref(sections[i]);
    }
}

/* Cases. */

static void show_recent(App *app) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->recent_list)))
        gtk_list_box_remove(GTK_LIST_BOX(app->recent_list), child);
    g_menu_remove_all(app->recent_menu);
    char **paths = recent_paths(app);
    GMenu *items = g_menu_new(), *clear = g_menu_new();
    for (char **path = paths; *path; path++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(row), glyph(0xE8B7, "\xe2\x96\xa3", "#97bad5"));
        GtkWidget *label = gtk_label_new(*path);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(row), label);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(*path), g_free);
        gtk_list_box_append(GTK_LIST_BOX(app->recent_list), row);
        add_text_action(items, *path, "win.recent", *path);
    }
    if (*paths)
        g_menu_append(clear, "Clear recent cases", "win.clear-recent");
    g_menu_append_section(app->recent_menu, NULL, G_MENU_MODEL(items));
    g_menu_append_section(app->recent_menu, NULL, G_MENU_MODEL(clear));
    g_object_unref(items);
    g_object_unref(clear);
    g_strfreev(paths);
}

static void remember_case(App *app) {
    char **paths = recent_paths(app);
    GPtrArray *kept = g_ptr_array_new();
    g_ptr_array_add(kept, app->path);
    for (char **path = paths; *path && kept->len < 8; path++)
        if (strcmp(*path, app->path))
            g_ptr_array_add(kept, *path);
    g_key_file_set_string_list(app->settings, "cases", "recentCases", (const char *const *)kept->pdata, kept->len);
    g_ptr_array_free(kept, TRUE);
    g_strfreev(paths);
    settings_save(app);
    show_recent(app);
}

// Show the open case quietly.
static void update_case_bar(App *app) {
    gboolean open = app->db != NULL;
    gtk_widget_set_visible(app->welcome_case, open);
    if (!open) {
        gtk_window_set_title(app->window, APP_TITLE);
        gtk_label_set_text(GTK_LABEL(app->case_name), "No case open");
        gtk_widget_add_css_class(app->case_name, "none");
        gtk_widget_set_tooltip_text(app->case_name, "Create or open a case file (.bec) to begin.");
        gtk_label_set_text(GTK_LABEL(app->case_saved), "");
        return;
    }
    char *name = g_path_get_basename(app->path);
    char *title = g_strdup_printf("%s - %s", name, APP_TITLE);
    gtk_window_set_title(app->window, title);
    gtk_label_set_text(GTK_LABEL(app->case_name), name);
    gtk_widget_remove_css_class(app->case_name, "none");
    gtk_widget_set_tooltip_text(app->case_name, app->path);
    gtk_label_set_text(GTK_LABEL(app->welcome_title), name);
    gtk_label_set_text(GTK_LABEL(app->welcome_text), app->path);
    g_free(title);
    g_free(name);
}

static void rebuild_log_menu(App *app) {
    g_menu_remove_all(app->log_menu);
    GMenu *all = g_menu_new(), *logs = g_menu_new();
    g_menu_append(all, "All case events", "win.all");
    for (guint i = 0; i < app->logs->len; i++) {
        LogInfo *log = app->logs->pdata[i];
        char *name = app_log_name(log), *target = g_strdup_printf("%" G_GINT64_FORMAT, log->id);
        add_text_action(logs, name, "win.tab", target);
        g_free(name);
        g_free(target);
    }
    g_menu_append_section(app->log_menu, NULL, G_MENU_MODEL(all));
    g_menu_append_section(app->log_menu, NULL, G_MENU_MODEL(logs));
    g_object_unref(all);
    g_object_unref(logs);
}

static void refresh_logs(App *app) {
    g_ptr_array_set_size(app->logs, 0);
    sqlite3_stmt *rows = app->db ? sql_prepare(app->db, "SELECT id,file,name,sheet,kind,total,warning FROM logs ORDER BY id", NULL)
                                 : NULL;
    while (rows && sqlite3_step(rows) == SQLITE_ROW) {
        LogInfo *log = g_new0(LogInfo, 1);
        log->id = sqlite3_column_int64(rows, 0);
        log->file = sqlite3_column_int64(rows, 1);
        log->name = g_strdup(column_text(rows, 2));
        log->sheet = g_strdup(column_text(rows, 3));
        log->kind = g_strdup(column_text(rows, 4));
        log->total = sqlite3_column_int64(rows, 5);
        log->warning = g_strdup(column_text(rows, 6));
        g_ptr_array_add(app->logs, log);
    }
    sqlite3_finalize(rows);
    rebuild_log_menu(app);
    update_case_bar(app);
}

static void remove_tabs(App *app) {
    app->switching = TRUE;
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->tabs)))
        gtk_notebook_remove_page(GTK_NOTEBOOK(app->tabs), 0);
    app->switching = FALSE;
}

static void close_case(App *app) {
    if (!app->db || app_busy(app))
        return;
    cancel_query(app);
    timeline_reset(app->timeline, NULL, NULL);
    inspector_clear(app->inspector);
    statistics_close_all(app);
    timeline_clear_columns(app->timeline);
    char *name = g_path_get_basename(app->path);
    sqlite3_close(app->db);
    app->db = NULL;
    g_clear_pointer(&app->path, g_free);
    app->has_active = FALSE;
    g_hash_table_remove_all(app->views);
    g_array_set_size(app->closed_tabs, 0);
    g_ptr_array_set_size(app->history, 0);
    remove_tabs(app);
    refresh_logs(app);
    go_home(app);
    char *message = g_strdup_printf("Closed case %s. All changes were saved.", name);
    app_message(app, message);
    g_free(message);
    g_free(name);
}

gboolean app_load(App *app, const char *path, gboolean create) {
    if (app_busy(app))
        return FALSE;
    char *absolute = absolute_path(path);
    if (!create && !g_file_test(absolute, G_FILE_TEST_IS_REGULAR)) {
        app_alert(app, "Open case", "Case file could not be found.");
        g_free(absolute);
        return FALSE;
    }
    GError *error = NULL;
    sqlite3 *db = case_connect(absolute, &error);
    if (!db) {
        app_alert(app, "Open case", error->message);
        g_clear_error(&error);
        g_free(absolute);
        return FALSE;
    }
    cancel_query(app);
    timeline_reset(app->timeline, NULL, NULL);
    inspector_clear(app->inspector);
    statistics_close_all(app);
    if (app->db)
        sqlite3_close(app->db);
    timeline_clear_columns(app->timeline);
    app->db = db;
    g_free(app->path);
    app->path = absolute;
    app->has_active = FALSE;
    g_hash_table_remove_all(app->views);
    g_array_set_size(app->closed_tabs, 0);
    g_ptr_array_set_size(app->history, 0);
    remove_tabs(app);
    refresh_logs(app);
    remember_case(app);
    char *name = g_path_get_basename(app->path);
    gtk_label_set_text(GTK_LABEL(app->case_saved), create ? "New case created" : "");
    if (app->logs->len) {
        make_tab(app, 0);
        for (guint i = 0; i < app->logs->len; i++)
            make_tab(app, ((LogInfo *)app->logs->pdata[i])->id);
        app_tab(app, 0);
    } else {
        go_home(app);
        char *message = create ? g_strdup_printf("Created case %s. Use Open logs to add evidence.", name)
                               : g_strdup_printf("Opened case %s. It holds no logs yet.", name);
        app_message(app, message);
        g_free(message);
    }
    g_free(name);
    return TRUE;
}

static void go_home(App *app) {
    remember_view(app);
    app->has_active = FALSE;
    cancel_query(app);
    timeline_reset(app->timeline, NULL, NULL);
    inspector_clear(app->inspector);
    gtk_stack_set_visible_child_name(GTK_STACK(app->pages), "welcome");
    gtk_editable_set_text(GTK_EDITABLE(app->entry), "");
    app_message(app, "Ready to open email logs");
    gtk_widget_set_visible(app->warnings, FALSE);
    char *scope = app->db ? g_strdup_printf("%u logs in case", app->logs->len) : g_strdup("No case open");
    gtk_label_set_text(GTK_LABEL(app->scope), scope);
    g_free(scope);
    update_actions(app);
}

/* File jobs run off-thread. */

typedef struct {
    App *app;
    char *path, *kind, *destination, *query, *order;
    GPtrArray *paths, *custom, *failed, *skipped;
    gint64 log, file;
    gboolean desc;
    guint imported;
    char *error;
} FileJob;

typedef struct {
    App *app;
    char *text;
} Progress;

static gboolean progress_show(gpointer data) {
    Progress *progress = data;
    app_message(progress->app, progress->text);
    g_free(progress->text);
    g_free(progress);
    return G_SOURCE_REMOVE;
}

static void post_progress(const char *message, gpointer user) {
    Progress *progress = g_new0(Progress, 1);
    progress->app = user;
    progress->text = g_strdup(message);
    g_idle_add(progress_show, progress);
}

static void custom_free(gpointer data) {
    CustomColumn *column = data;
    g_free(column->label);
    g_free(column->path);
    g_free(column);
}

// Rereads logs, returning new ones.
static GPtrArray *reload_logs(App *app) {
    GHashTable *before = g_hash_table_new(g_int64_hash, g_int64_equal);
    GArray *known = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint i = 0; i < app->logs->len; i++)
        g_array_append_val(known, ((LogInfo *)app->logs->pdata[i])->id);
    for (guint i = 0; i < known->len; i++)
        g_hash_table_add(before, &g_array_index(known, gint64, i));
    refresh_logs(app);
    GPtrArray *added = g_ptr_array_new();
    for (guint i = 0; i < app->logs->len; i++)
        if (!g_hash_table_contains(before, &((LogInfo *)app->logs->pdata[i])->id))
            g_ptr_array_add(added, app->logs->pdata[i]);
    if (app->logs->len)
        make_tab(app, 0);
    for (guint i = 0; i < added->len; i++)
        make_tab(app, ((LogInfo *)added->pdata[i])->id);
    g_array_free(known, TRUE);
    g_hash_table_destroy(before);
    return added;
}

static void import_finished(App *app, FileJob *job) {
    GPtrArray *added = reload_logs(app);
    if (added->len)
        app_tab(app, added->len == 1 ? ((LogInfo *)added->pdata[0])->id : 0);
    GString *warnings = g_string_new(NULL);
    for (guint i = 0; i < added->len; i++) {
        LogInfo *log = added->pdata[i];
        if (*log->warning)
            g_string_append_printf(warnings, "%s: %s\n", log->name, log->warning);
    }
    if (warnings->len) {
        g_string_append(warnings, "\nCollect smaller UTC windows or use reporting APIs.");
        app_alert(app, "Possible export truncation", warnings->str);
    }
    if (job->failed->len) {
        GString *text = g_string_new(NULL);
        for (guint i = 0; i < job->failed->len; i++)
            g_string_append_printf(text, "%s%s", i ? "\n" : "", (char *)job->failed->pdata[i]);
        g_string_append(text, "\n\nThe case was left unchanged for these files.");
        app_alert(app, "Files not imported", text->str);
        g_string_free(text, TRUE);
    }
    if (job->skipped->len && !added->len) {
        char *message = g_strdup_printf("%u unchanged file(s) already in this case", job->skipped->len);
        app_message(app, message);
        g_free(message);
    }
    if (job->imported) {
        char *what = g_strdup_printf("%u evidence file(s) added", job->imported);
        statistics_refresh_all(app);
        app_saved(app, what);
        g_free(what);
    }
    g_string_free(warnings, TRUE);
    g_ptr_array_free(added, TRUE);
    update_actions(app);
}

static gboolean file_job_done(gpointer data) {
    FileJob *job = data;
    App *app = job->app;
    app->busy = FALSE;
    if (job->error) {
        app_alert(app, APP_TITLE, job->error);
        app_message(app, job->error);
    } else if (!strcmp(job->kind, "import")) {
        import_finished(app, job);
    } else {
        char *message = g_strdup_printf("Saved %s", job->destination);
        app_message(app, message);
        g_free(message);
    }
    if (job->paths)
        g_ptr_array_free(job->paths, TRUE);
    if (job->custom)
        g_ptr_array_free(job->custom, TRUE);
    g_ptr_array_free(job->failed, TRUE);
    g_ptr_array_free(job->skipped, TRUE);
    g_free(job->path);
    g_free(job->kind);
    g_free(job->destination);
    g_free(job->query);
    g_free(job->order);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer file_job_run(gpointer data) {
    FileJob *job = data;
    GError *error = NULL;
    sqlite3 *db = case_connect(job->path, &error);
    if (db && !strcmp(job->kind, "import")) {
        for (guint i = 0; i < job->paths->len; i++) {
            const char *path = job->paths->pdata[i];
            char *name = g_path_get_basename(path);
            char *opening = g_strconcat("Opening ", name, NULL);
            post_progress(opening, job->app);
            g_free(opening);
            GError *problem = NULL;
            ImportState state = import_file(db, path, post_progress, job->app, &problem);
            if (state == IMPORT_DUPLICATE)
                g_ptr_array_add(job->skipped, g_strdup(name));
            else if (state == IMPORT_DONE)
                job->imported++;
            else
                g_ptr_array_add(job->failed, g_strdup_printf("%s \xc2\xb7 %s", name, problem ? problem->message : "failed"));
            g_clear_error(&problem);
            g_free(name);
        }
    } else if (db && !strcmp(job->kind, "export")) {
        export_events(db, job->destination, job->log, job->query, job->desc, job->order, job->custom, NULL, &error);
    } else if (db) {
        extract_evidence(db, job->file, job->destination, &error);
    }
    if (db)
        sqlite3_close(db);
    if (error) {
        job->error = g_strdup(error->message);
        g_clear_error(&error);
    }
    g_idle_add(file_job_done, job);
    return NULL;
}

static FileJob *file_job(App *app, const char *kind) {
    FileJob *job = g_new0(FileJob, 1);
    job->app = app;
    job->path = g_strdup(app->path);
    job->kind = g_strdup(kind);
    job->failed = g_ptr_array_new_with_free_func(g_free);
    job->skipped = g_ptr_array_new_with_free_func(g_free);
    return job;
}

static void start_job(App *app, FileJob *job) {
    app->busy = TRUE;
    g_thread_unref(g_thread_new("file-job", file_job_run, job));
}

void app_import(App *app, GPtrArray *paths) {
    if (!app->db || app_busy(app)) {
        g_ptr_array_free(paths, TRUE);
        return;
    }
    FileJob *job = file_job(app, "import");
    job->paths = paths;
    start_job(app, job);
}

/* File choosers. */

static void add_filter(GListStore *filters, const char *name, const char *const *patterns) {
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, name);
    for (; *patterns; patterns++)
        gtk_file_filter_add_pattern(filter, *patterns);
    g_list_store_append(filters, filter);
    g_object_unref(filter);
}

static GtkFileDialog *file_dialog(const char *title, const char *name, const char *const *patterns) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_modal(dialog, TRUE);
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    add_filter(filters, name, patterns);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    return dialog;
}

typedef struct {
    App *app;
    GPtrArray *pending;
} NewCase;

static void new_case_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    NewCase *request = data;
    App *app = request->app;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    gboolean created = FALSE;
    if (file) {
        char *chosen = g_file_get_path(file);
        char *path = g_str_has_suffix(chosen, ".bec") ? g_strdup(chosen) : g_strconcat(chosen, ".bec", NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            app_alert(app, APP_TITLE, "Choose an unused name, or use Open case.");
        else
            created = app_load(app, path, TRUE);
        g_free(path);
        g_free(chosen);
        g_object_unref(file);
    }
    if (created && request->pending) {
        app_import(app, request->pending);
        request->pending = NULL;
    }
    if (request->pending)
        g_ptr_array_free(request->pending, TRUE);
    g_free(request);
}

static void new_case(App *app, GPtrArray *pending) {
    if (app_busy(app)) {
        if (pending)
            g_ptr_array_free(pending, TRUE);
        return;
    }
    GtkFileDialog *dialog = file_dialog(pending ? "Save a new case file to hold these logs" : "Create a new case file",
                                        "Investigation case (*.bec)", (const char *[]){"*.bec", NULL});
    GDateTime *now = g_date_time_new_now_local();
    char *name = g_date_time_format(now, "Investigation %Y-%m-%d.bec");
    gtk_file_dialog_set_initial_name(dialog, name);
    g_free(name);
    g_date_time_unref(now);
    const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    if (documents) {
        GFile *folder = g_file_new_for_path(documents);
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    NewCase *request = g_new0(NewCase, 1);
    request->app = app;
    request->pending = pending;
    gtk_file_dialog_save(dialog, app->window, NULL, new_case_chosen, request);
    g_object_unref(dialog);
}

static void create_for_logs(App *app, gboolean accepted, gpointer user) {
    GPtrArray *paths = user;
    if (accepted)
        new_case(app, paths);
    else
        g_ptr_array_free(paths, TRUE);
}

void app_open_files_then(App *app, GPtrArray *paths) {
    if (app->db) {
        app_import(app, paths);
        return;
    }
    char *detail = g_strdup_printf(
        "%u log file(s) selected. Logs are copied into a case file (.bec) that keeps your marks, filters and sealed "
        "copies of the evidence. Your original log files are only read, never changed.\n\nChoose where to save the new case.",
        paths->len);
    dialog_confirm(app, "Create a case for these logs", detail, "Create case\xe2\x80\xa6", create_for_logs, paths);
    g_free(detail);
}

static void case_chosen(GObject *source, GAsyncResult *result, gpointer user) {
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file)
        return;
    char *path = g_file_get_path(file);
    app_load(user, path, FALSE);
    g_free(path);
    g_object_unref(file);
}

static void logs_chosen(GObject *source, GAsyncResult *result, gpointer user) {
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!files)
        return;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        GFile *file = g_list_model_get_item(files, i);
        g_ptr_array_add(paths, g_file_get_path(file));
        g_object_unref(file);
    }
    g_object_unref(files);
    if (paths->len)
        app_open_files_then(user, paths);
    else
        g_ptr_array_free(paths, TRUE);
}

static void export_chosen(GObject *source, GAsyncResult *result, gpointer user) {
    App *app = user;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file || !app->has_active || app->busy)
        return;
    char *chosen = g_file_get_path(file);
    g_object_unref(file);
    char *base = g_path_get_basename(chosen);
    FileJob *job = file_job(app, "export");
    job->destination = strchr(base, '.') ? g_strdup(chosen) : g_strconcat(chosen, ".csv", NULL);
    View *view = app_view(app);
    job->log = app->active;
    job->query = g_strdup(view->query);
    job->order = g_strdup(view->order);
    job->desc = view->desc;
    job->custom = g_ptr_array_new_with_free_func(custom_free);
    GPtrArray *custom = timeline_custom(app->timeline);
    for (guint i = 0; i < custom->len; i++) {
        CustomColumn *copy = g_new0(CustomColumn, 1);
        copy->label = g_strdup(((CustomColumn *)custom->pdata[i])->label);
        copy->path = g_strdup(((CustomColumn *)custom->pdata[i])->path);
        g_ptr_array_add(job->custom, copy);
    }
    app_message(app, "Exporting displayed events\xe2\x80\xa6");
    start_job(app, job);
    g_free(base);
    g_free(chosen);
}

static void job_free(FileJob *job) {
    if (job->paths)
        g_ptr_array_free(job->paths, TRUE);
    if (job->custom)
        g_ptr_array_free(job->custom, TRUE);
    g_ptr_array_free(job->failed, TRUE);
    g_ptr_array_free(job->skipped, TRUE);
    g_free(job->path);
    g_free(job->kind);
    g_free(job->destination);
    g_free(job->query);
    g_free(job->order);
    g_free(job->error);
    g_free(job);
}

static void extract_chosen(GObject *source, GAsyncResult *result, gpointer user) {
    FileJob *job = user;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file || job->app->busy) {
        g_clear_object(&file);
        job_free(job);
        return;
    }
    job->destination = g_file_get_path(file);
    g_object_unref(file);
    start_job(job->app, job);
}

/* Actions. */

static GArray *selected_ids(App *app) {
    GArray *positions = timeline_selected(app->timeline);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint i = 0; i < positions->len; i++) {
        gint64 id = timeline_id(app->timeline, g_array_index(positions, guint, i));
        g_array_append_val(ids, id);
    }
    g_array_free(positions, TRUE);
    return ids;
}

static void verdict_applied(App *app, GError *error, const char *what) {
    if (error) {
        app_alert(app, APP_TITLE, error->message);
        g_clear_error(&error);
        return;
    }
    app_saved(app, what);
    statistics_refresh_all(app);
    app_refresh(app);
}

typedef struct {
    GPtrArray *ips;
    char *verdict;
} IpMark;

static void ips_confirmed(App *app, gboolean accepted, gpointer user) {
    IpMark *mark = user;
    if (accepted && app->db) {
        GPtrArray *pairs = g_ptr_array_new();
        for (guint i = 0; i < mark->ips->len; i++) {
            g_ptr_array_add(pairs, mark->ips->pdata[i]);
            g_ptr_array_add(pairs, "");
        }
        GError *error = NULL;
        remember_view(app);
        assign_verdict(app->db, "ip", pairs, mark->verdict, &error);
        verdict_applied(app, error, "IP verdict");
        g_ptr_array_free(pairs, TRUE);
    }
    g_ptr_array_free(mark->ips, TRUE);
    g_free(mark->verdict);
    g_free(mark);
}

static int compare_text(gconstpointer a, gconstpointer b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void mark(App *app, const char *scope, const char *verdict) {
    if (!app->has_active || app_busy(app))
        return;
    if (!strcmp(scope, "range")) {
        dialog_mark_time(app, verdict);
        return;
    }
    if (!strcmp(scope, "events")) {
        GArray *ids = selected_ids(app);
        if (ids->len) {
            GPtrArray *pairs = g_ptr_array_new_with_free_func(g_free);
            for (guint i = 0; i < ids->len; i++) {
                g_ptr_array_add(pairs, g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(ids, gint64, i)));
                g_ptr_array_add(pairs, g_strdup(""));
            }
            GError *error = NULL;
            remember_view(app);
            assign_verdict(app->db, "event", pairs, verdict, &error);
            verdict_applied(app, error, "event verdict");
            g_ptr_array_free(pairs, TRUE);
        }
        g_array_free(ids, TRUE);
        return;
    }
    GPtrArray *ips = g_ptr_array_new_with_free_func(g_free);
    GArray *positions = timeline_selected(app->timeline);
    for (guint i = 0; i < positions->len; i++) {
        Record *record = timeline_record(app->timeline, g_array_index(positions, guint, i));
        char **parts = record ? g_strsplit(record->ip, ", ", -1) : NULL;
        for (char **part = parts; part && *part; part++) {
            gboolean known = !**part;
            for (guint k = 0; k < ips->len && !known; k++)
                known = !strcmp(ips->pdata[k], *part);
            if (!known)
                g_ptr_array_add(ips, g_strdup(*part));
        }
        g_strfreev(parts);
    }
    g_array_free(positions, TRUE);
    if (!ips->len) {
        g_ptr_array_free(ips, TRUE);
        app_alert(app, APP_TITLE, "Select an event with an IP address.");
        return;
    }
    g_ptr_array_sort(ips, compare_text);
    GString *detail = g_string_new(NULL);
    g_string_printf(detail, "Apply %s to all matching events in this case?\n", verdict);
    for (guint i = 0; i < ips->len && i < 20; i++)
        g_string_append_printf(detail, "\n%s", (char *)ips->pdata[i]);
    if (ips->len > 20)
        g_string_append_printf(detail, "\n\xe2\x80\xa6 and %u more", ips->len - 20);
    IpMark *request = g_new0(IpMark, 1);
    request->ips = ips;
    request->verdict = g_strdup(verdict);
    dialog_confirm(app, "Mark selected IPs", detail->str, "Yes", ips_confirmed, request);
    g_string_free(detail, TRUE);
}

static void copy_events(App *app) {
    GArray *positions = timeline_selected(app->timeline);
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < positions->len; i++) {
        Record *r = timeline_record(app->timeline, g_array_index(positions, guint, i));
        if (!r)
            continue;
        g_string_append_printf(out, "%s%" G_GINT64_FORMAT "\t%s\t%s\t%s\t%s\t%" G_GINT64_FORMAT "\t%s\t%s\t%s\t%s\t%s",
                               out->len ? "\n" : "", r->number, r->time, r->source, r->name, r->sheet, r->rownum, r->actor,
                               r->ip, r->op, r->result, r->verdict);
    }
    if (positions->len)
        copy_text(GTK_WIDGET(app->window), out->str);
    g_string_free(out, TRUE);
    g_array_free(positions, TRUE);
}

static void navigate(App *app, const char *step) {
    guint count = timeline_count(app->timeline);
    if (!count)
        return;
    int position = timeline_current(app->timeline);
    if (!strcmp(step, "first"))
        position = 0;
    else if (!strcmp(step, "last"))
        position = (int)count - 1;
    else
        position += !strcmp(step, "next") ? 1 : -1;
    timeline_select(app->timeline, (guint)CLAMP(position, 0, (int)count - 1));
}

static void apply_zoom(App *app, int delta) {
    app->zoom = delta ? CLAMP(app->zoom + delta, 7, 16) : 9;
    char *css = g_strdup_printf("columnview { font-size: %dpt; }", app->zoom);
    gtk_css_provider_load_from_string(app->zoom_css, css);
    g_free(css);
}

static void show_panes(App *app) {
    gboolean details = setting_bool(app, "detailsPaneVisible", TRUE), source = setting_bool(app, "sourcePaneVisible", TRUE);
    gtk_widget_set_visible(inspector_pane(app->inspector, 0), details);
    gtk_widget_set_visible(inspector_pane(app->inspector, 1), source);
    gtk_widget_set_visible(inspector_widget(app->inspector), details || source);
}

void app_set_pane(App *app, const char *key, gboolean visible) {
    char *name = g_strconcat("pane-", key, NULL);
    g_action_group_change_action_state(G_ACTION_GROUP(app->window), name, g_variant_new_boolean(visible));
    g_free(name);
}

static void activate(GSimpleAction *action, GVariant *parameter, gpointer user) {
    App *app = user;
    const char *name = g_action_get_name(G_ACTION(action));
    const char *text = parameter ? g_variant_get_string(parameter, NULL) : NULL;
    if (!strcmp(name, "open")) {
        if (app_busy(app))
            return;
        GtkFileDialog *dialog = file_dialog(app->db ? "Add logs to this case" : "Open logs from one tenant",
                                            "Log files (*.csv *.xlsx *.json)", (const char *[]){"*.csv", "*.xlsx", "*.json", NULL});
        gtk_file_dialog_open_multiple(dialog, app->window, NULL, logs_chosen, app);
        g_object_unref(dialog);
    } else if (!strcmp(name, "new")) {
        new_case(app, NULL);
    } else if (!strcmp(name, "case")) {
        if (app_busy(app))
            return;
        GtkFileDialog *dialog = file_dialog("Open case", "Investigation case (*.bec)", (const char *[]){"*.bec", NULL});
        gtk_file_dialog_open(dialog, app->window, NULL, case_chosen, app);
        g_object_unref(dialog);
    } else if (!strcmp(name, "close-case")) {
        close_case(app);
    } else if (!strcmp(name, "case-folder")) {
        if (app->path) {
            GFile *file = g_file_new_for_path(app->path);
            GtkFileLauncher *launcher = gtk_file_launcher_new(file);
            gtk_file_launcher_open_containing_folder(launcher, app->window, NULL, NULL, NULL);
            g_object_unref(launcher);
            g_object_unref(file);
        }
    } else if (!strcmp(name, "recent")) {
        app_load(app, text, FALSE);
    } else if (!strcmp(name, "clear-recent")) {
        g_key_file_remove_key(app->settings, "cases", "recentCases", NULL);
        settings_save(app);
        show_recent(app);
    } else if (!strcmp(name, "export")) {
        if (!app->has_active || app_busy(app))
            return;
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Export displayed events");
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        add_filter(filters, "CSV (*.csv)", (const char *[]){"*.csv", NULL});
        add_filter(filters, "Excel workbook (*.xlsx)", (const char *[]){"*.xlsx", NULL});
        add_filter(filters, "JSON (*.json)", (const char *[]){"*.json", NULL});
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(filters);
        gtk_file_dialog_set_initial_name(dialog, "displayed-events.csv");
        gtk_file_dialog_save(dialog, app->window, NULL, export_chosen, app);
        g_object_unref(dialog);
    } else if (!strcmp(name, "extract")) {
        if (!app->has_active || app_busy(app))
            return;
        gint64 key = app->active;
        int current = timeline_current(app->timeline);
        if (!key && current >= 0) {
            char *sql = g_strdup_printf("SELECT log FROM events WHERE id=%" G_GINT64_FORMAT, timeline_id(app->timeline, (guint)current));
            key = sql_int(app->db, sql, NULL);
            g_free(sql);
        }
        LogInfo *log = key > 0 ? app_log(app, key) : NULL;
        if (!log) {
            app_alert(app, APP_TITLE, "Select a log tab or an event.");
            return;
        }
        FileJob *job = file_job(app, "extract");
        job->file = log->file;
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Save original evidence copy");
        char *initial = g_strconcat("copy-", log->name, NULL);
        gtk_file_dialog_set_initial_name(dialog, initial);
        g_free(initial);
        gtk_file_dialog_save(dialog, app->window, NULL, extract_chosen, job);
        g_object_unref(dialog);
    } else if (!strcmp(name, "close")) {
        if (app->has_active)
            close_index(app, gtk_notebook_get_current_page(GTK_NOTEBOOK(app->tabs)));
    } else if (!strcmp(name, "quit")) {
        gtk_window_close(app->window);
    } else if (!strcmp(name, "copy")) {
        copy_events(app);
    } else if (g_str_has_prefix(name, "mark-")) {
        mark(app, name + 5, text);
    } else if (!strcmp(name, "undo")) {
        if (!app->db || app_busy(app))
            return;
        GError *error = NULL;
        undo_verdict(app->db, &error);
        if (app->has_active) {
            remember_view(app);
            verdict_applied(app, error, "undo");
        } else {
            g_clear_error(&error);
            statistics_refresh_all(app);
        }
    } else if (!strcmp(name, "home")) {
        go_home(app);
    } else if (!strcmp(name, "all")) {
        app_tab(app, 0);
    } else if (!strcmp(name, "tab")) {
        app_tab(app, g_ascii_strtoll(text, NULL, 10));
    } else if (!strcmp(name, "reopen")) {
        if (app->closed_tabs->len)
            app_tab(app, g_array_index(app->closed_tabs, gint64, app->closed_tabs->len - 1));
    } else if (!strcmp(name, "zoom-in")) {
        apply_zoom(app, 1);
    } else if (!strcmp(name, "zoom-out")) {
        apply_zoom(app, -1);
    } else if (!strcmp(name, "zoom-reset")) {
        apply_zoom(app, 0);
    } else if (!strcmp(name, "previous") || !strcmp(name, "next") || !strcmp(name, "first") || !strcmp(name, "last")) {
        navigate(app, name);
    } else if (!strcmp(name, "focus")) {
        gtk_widget_grab_focus(app->entry);
        gtk_editable_select_region(GTK_EDITABLE(app->entry), 0, -1);
    } else if (!strcmp(name, "find")) {
        app_set_pane(app, "source", TRUE);
        inspector_focus_find(app->inspector);
    } else if (!strcmp(name, "clear")) {
        app_apply(app, "");
    } else if (!strcmp(name, "apply")) {
        app_apply(app, NULL);
    } else if (!strcmp(name, "save-filter")) {
        dialog_save_filter(app);
    } else if (!strcmp(name, "saved-filters")) {
        dialog_saved_filters(app);
    } else if (!strcmp(name, "quality")) {
        dialog_import_details(app);
    } else if (!strcmp(name, "stat")) {
        statistics_open(app, text);
    } else if (!strcmp(name, "help")) {
        dialog_help(app);
    } else if (!strcmp(name, "filter-apply")) {
        app_apply(app, text);
    } else if (!strcmp(name, "filter-prepare")) {
        app_prepare(app, text);
    } else if (!strcmp(name, "filter-case")) {
        app_apply_in(app, 0, text);
    } else if (!strcmp(name, "copy-text")) {
        copy_text(GTK_WIDGET(app->window), text);
    } else if (!strcmp(name, "add-column")) {
        char **parts = g_strsplit(text, "\x1f", 2);
        if (parts[0] && parts[1]) {
            timeline_add_column(app->timeline, parts[0], parts[1]);
            char *message = g_strdup_printf("Added column \xc2\xb7 %s", parts[1]);
            app_message(app, message);
            g_free(message);
        }
        g_strfreev(parts);
    } else if (!strcmp(name, "follow-event")) {
        char *sql = g_strdup_printf("SELECT log,rownum FROM events WHERE id=%" G_GINT64_FORMAT, g_ascii_strtoll(text, NULL, 10));
        sqlite3_stmt *row = sql_prepare(app->db, sql, NULL);
        if (row && sqlite3_step(row) == SQLITE_ROW) {
            char *filter = g_strdup_printf("row = %" G_GINT64_FORMAT, (gint64)sqlite3_column_int64(row, 1));
            gint64 log = sqlite3_column_int64(row, 0);
            sqlite3_finalize(row);
            row = NULL;
            app_apply_in(app, log, filter);
            g_free(filter);
        }
        sqlite3_finalize(row);
        g_free(sql);
    }
}

static void change_state(GSimpleAction *action, GVariant *state, gpointer user) {
    App *app = user;
    const char *name = g_action_get_name(G_ACTION(action));
    gboolean on = g_variant_get_boolean(state);
    g_simple_action_set_state(action, state);
    if (!strcmp(name, "pane-details") || !strcmp(name, "pane-source")) {
        setting_set_bool(app, !strcmp(name, "pane-details") ? "detailsPaneVisible" : "sourcePaneVisible", on);
        show_panes(app);
    } else if (!strcmp(name, "colors")) {
        setting_set_bool(app, "verdictColors", on);
        timeline_set_colors(app->timeline, on);
    } else if (!strcmp(name, "stack")) {
        setting_set_bool(app, "inspectorVertical", on);
        inspector_set_vertical(app->inspector, on);
    }
}

static void set_enabled(App *app, const char *name, gboolean enabled) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(app->window), name);
    if (action)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void update_actions(App *app) {
    gboolean active = app->has_active && app->db;
    static const char *const needs_view[] = {"stat",     "export", "close", "copy", "mark-events", "mark-ips", "mark-range",
                                             "previous", "next",   "first", "last", "find",        "extract"};
    for (guint i = 0; i < G_N_ELEMENTS(needs_view); i++)
        set_enabled(app, needs_view[i], active);
    static const char *const needs_case[] = {"all", "quality", "undo", "save-filter", "saved-filters", "close-case", "case-folder"};
    for (guint i = 0; i < G_N_ELEMENTS(needs_case); i++)
        set_enabled(app, needs_case[i], app->db != NULL);
    set_enabled(app, "reopen", app->closed_tabs->len > 0);
}

/* Menus and toolbar. */

static void item(GMenu *menu, const char *label, const char *action) {
    g_menu_append(menu, label, action);
}

static void mark_submenu(GMenu *menu, const char *title, const char *action) {
    GMenu *sub = g_menu_new();
    for (int i = 0; i < 4; i++)
        add_text_action(sub, i == 3 ? "Clear verdict" : VERDICTS[i], action, VERDICTS[i]);
    g_menu_append_submenu(menu, title, G_MENU_MODEL(sub));
    g_object_unref(sub);
}

static void stat_item(GMenu *menu, const char *key) {
    char *label = g_strconcat(report_find(key)->title, "\xe2\x80\xa6", NULL);
    add_text_action(menu, label, "win.stat", key);
    g_free(label);
}

static GMenuModel *build_menus(App *app) {
    GMenu *bar = g_menu_new();
    GMenu *file = g_menu_new(), *section = g_menu_new();
    item(section, "Open logs\xe2\x80\xa6", "win.open");
    item(section, "New case\xe2\x80\xa6", "win.new");
    item(section, "Open case\xe2\x80\xa6", "win.case");
    g_menu_append_submenu(section, "Recent cases", G_MENU_MODEL(app->recent_menu));
    item(section, "Close case", "win.close-case");
    item(section, "Show case file in folder", "win.case-folder");
    g_menu_append_section(file, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    item(section, "Export displayed events\xe2\x80\xa6", "win.export");
    item(section, "Save original evidence copy\xe2\x80\xa6", "win.extract");
    item(section, "Close tab", "win.close");
    g_menu_append_section(file, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    item(section, "Exit", "win.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(file));
    g_object_unref(file);

    GMenu *edit = g_menu_new();
    item(edit, "Copy event with source reference", "win.copy");
    mark_submenu(edit, "Mark selected events", "win.mark-events");
    mark_submenu(edit, "Mark selected IPs", "win.mark-ips");
    mark_submenu(edit, "Mark UTC time range", "win.mark-range");
    item(edit, "Undo last mark", "win.undo");
    g_menu_append_submenu(bar, "_Edit", G_MENU_MODEL(edit));
    g_object_unref(edit);

    GMenu *view = g_menu_new();
    section = g_menu_new();
    item(section, "Welcome screen", "win.home");
    item(section, "All case events", "win.all");
    g_menu_append_submenu(section, "Open log tab", G_MENU_MODEL(app->log_menu));
    item(section, "Reopen closed tab", "win.reopen");
    g_menu_append_section(view, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    item(section, "Event details", "win.pane-details");
    item(section, "Source record", "win.pane-source");
    item(section, "Show verdict colors", "win.colors");
    item(section, "Stack inspector panes vertically", "win.stack");
    item(section, "Zoom in", "win.zoom-in");
    item(section, "Zoom out", "win.zoom-out");
    item(section, "Actual size", "win.zoom-reset");
    g_menu_append_section(view, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    g_menu_append_submenu(bar, "_View", G_MENU_MODEL(view));
    g_object_unref(view);

    GMenu *go = g_menu_new();
    item(go, "Previous event", "win.previous");
    item(go, "Next event", "win.next");
    item(go, "First displayed event", "win.first");
    item(go, "Last displayed event", "win.last");
    g_menu_append_submenu(bar, "_Go", G_MENU_MODEL(go));
    g_object_unref(go);

    GMenu *analyze = g_menu_new();
    item(analyze, "Display filter", "win.focus");
    item(analyze, "Find in source record", "win.find");
    item(analyze, "Clear display filter", "win.clear");
    item(analyze, "Save display filter in case\xe2\x80\xa6", "win.save-filter");
    item(analyze, "Saved display filters\xe2\x80\xa6", "win.saved-filters");
    item(analyze, "Evidence and import details\xe2\x80\xa6", "win.quality");
    g_menu_append_submenu(bar, "_Analyze", G_MENU_MODEL(analyze));
    g_object_unref(analyze);

    GMenu *statistics = g_menu_new();
    static const char *const main_reports[] = {"activity",  "indicators", "sources",     "files",        "operations",
                                               "endpoints", "mailboxes",  "ip_accounts", "conversations"};
    for (guint i = 0; i < G_N_ELEMENTS(main_reports); i++)
        stat_item(statistics, main_reports[i]);
    static const char *const identity[] = {"outcomes", "devices", "sessions", "locations", "applications", "clients", "methods",
                                           "policies"};
    static const char *const mailbox[] = {"rules", "access", "folders", "items", "delivery", "consent", "marks"};
    GMenu *sub = g_menu_new();
    for (guint i = 0; i < G_N_ELEMENTS(identity); i++)
        stat_item(sub, identity[i]);
    g_menu_append_submenu(statistics, "Identity and authentication", G_MENU_MODEL(sub));
    g_object_unref(sub);
    sub = g_menu_new();
    for (guint i = 0; i < G_N_ELEMENTS(mailbox); i++)
        stat_item(sub, mailbox[i]);
    g_menu_append_submenu(statistics, "Mailbox and directory", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append_submenu(bar, "_Statistics", G_MENU_MODEL(statistics));
    g_object_unref(statistics);

    GMenu *help = g_menu_new();
    item(help, "BQL display filter reference\xe2\x80\xa6", "win.help");
    g_menu_append_submenu(bar, "_Help", G_MENU_MODEL(help));
    g_object_unref(help);
    return G_MENU_MODEL(bar);
}

static GtkWidget *tool(App *app, guint code, const char *fallback, const char *color, const char *tip, const char *action,
                       const char *target) {
    GtkWidget *button = glyph_button(code, fallback, color, tip);
    if (target)
        gtk_actionable_set_action_target(GTK_ACTIONABLE(button), "s", target);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    gtk_box_append(GTK_BOX(app->toolbar), button);
    return button;
}

static void tool_menu(App *app, guint code, const char *fallback, const char *tip, const char *action) {
    GMenu *menu = g_menu_new();
    for (int i = 0; i < 4; i++)
        add_text_action(menu, i == 3 ? "Clear verdict" : VERDICTS[i], action, VERDICTS[i]);
    GtkWidget *button = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(button), glyph(code, fallback, NULL));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(button, "tool");
    gtk_widget_set_tooltip_text(button, tip);
    gtk_box_append(GTK_BOX(app->toolbar), button);
    g_object_unref(menu);
}

static void tool_gap(App *app) {
    gtk_box_append(GTK_BOX(app->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
}

static void build_toolbar(App *app) {
    app->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    gtk_widget_add_css_class(app->toolbar, "toolbar");
    tool(app, 0xE8E5, "\xe2\x96\xa4", "#e8bd61", "Open logs\xe2\x80\xa6", "win.open", NULL);
    tool(app, 0xE8B7, "\xe2\x96\xa3", "#98bfe2", "Open case\xe2\x80\xa6", "win.case", NULL);
    tool(app, 0xE8BB, "\xe2\x9c\x95", NULL, "Close tab", "win.close", NULL);
    tool_gap(app);
    tool(app, 0xE80F, "\xe2\x8c\x82", NULL, "Welcome screen", "win.home", NULL);
    tool(app, 0xE8A9, "\xe2\x96\xa6", "#99cbe6", "All case events", "win.all", NULL);
    tool_gap(app);
    tool(app, 0xE72B, "\xe2\x86\x90", NULL, "Previous event", "win.previous", NULL);
    tool(app, 0xE72A, "\xe2\x86\x92", NULL, "Next event", "win.next", NULL);
    tool(app, 0xE74A, "\xe2\x86\x91", NULL, "First displayed event", "win.first", NULL);
    tool(app, 0xE74B, "\xe2\x86\x93", NULL, "Last displayed event", "win.last", NULL);
    tool_gap(app);
    tool(app, 0xE721, "\xe2\x8c\x95", "#9fc8e9", "Display filter", "win.focus", NULL);
    GtkWidget *colors = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(colors), glyph(0xE790, "\xe2\x97\x90", NULL));
    gtk_widget_add_css_class(colors, "tool");
    gtk_widget_set_tooltip_text(colors, "Show verdict colors");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(colors), "win.colors");
    gtk_box_append(GTK_BOX(app->toolbar), colors);
    tool(app, 0xE9D9, "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x85", NULL, "Activity over time\xe2\x80\xa6", "win.stat", "activity");
    tool(app, 0xE946, "\xe2\x93\x98", NULL, "Evidence and import details\xe2\x80\xa6", "win.quality", NULL);
    tool_gap(app);
    tool_menu(app, 0xE7C1, "\xe2\x9a\x91", "Mark selected events", "win.mark-events");
    tool_menu(app, 0xE774, "\xe2\x97\x8d", "Mark selected IPs", "win.mark-ips");
    tool_menu(app, 0xE916, "\xe2\x97\xb7", "Mark UTC time range", "win.mark-range");
    tool(app, 0xE7A7, "\xe2\x86\xb6", NULL, "Undo last mark", "win.undo", NULL);
    tool_gap(app);
    tool(app, 0xE74E, "\xe2\xa4\x93", NULL, "Export displayed events\xe2\x80\xa6", "win.export", NULL);
    tool(app, 0xE8A3, "\xe2\x8a\x95", NULL, "Zoom in", "win.zoom-in", NULL);
    tool(app, 0xE71F, "\xe2\x8a\x96", NULL, "Zoom out", "win.zoom-out", NULL);
}

static GtkWidget *plain_button(const char *label, const char *action) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

static GtkWidget *left_label(const char *text, const char *css) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (css)
        gtk_widget_add_css_class(label, css);
    return label;
}

static void recent_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
    (void)list;
    GtkWidget *child = gtk_list_box_row_get_child(row);
    app_load(user, g_object_get_data(G_OBJECT(child), "path"), FALSE);
}

static GtkWidget *build_welcome(App *app) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(page, 56);
    gtk_widget_set_margin_end(page, 48);
    gtk_widget_set_margin_top(page, 34);
    gtk_widget_set_margin_bottom(page, 24);
    GtkWidget *banner = left_label("Welcome to " APP_TITLE, "banner");
    gtk_widget_set_halign(banner, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), banner);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_vexpand(row, TRUE);
    gtk_box_append(GTK_BOX(page), row);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(left, TRUE);
    app->welcome_case = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(app->welcome_case), left_label("Current case", "heading"));
    app->welcome_title = left_label("", "caseFile");
    gtk_box_append(GTK_BOX(app->welcome_case), app->welcome_title);
    app->welcome_text = left_label("", "secondary");
    gtk_label_set_ellipsize(GTK_LABEL(app->welcome_text), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(app->welcome_text), TRUE);
    gtk_box_append(GTK_BOX(app->welcome_case), app->welcome_text);
    GtkWidget *case_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_top(case_buttons, 10);
    gtk_box_append(GTK_BOX(case_buttons), plain_button("Add logs\xe2\x80\xa6", "win.open"));
    gtk_box_append(GTK_BOX(case_buttons), plain_button("All case events", "win.all"));
    gtk_box_append(GTK_BOX(case_buttons), plain_button("Close case", "win.close-case"));
    gtk_box_append(GTK_BOX(app->welcome_case), case_buttons);
    gtk_box_append(GTK_BOX(left), app->welcome_case);
    gtk_box_append(GTK_BOX(left), left_label("Open", "heading"));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(buttons), plain_button("Open logs\xe2\x80\xa6", "win.open"));
    gtk_box_append(GTK_BOX(buttons), plain_button("Open case\xe2\x80\xa6", "win.case"));
    gtk_box_append(GTK_BOX(buttons), plain_button("New case\xe2\x80\xa6", "win.new"));
    gtk_box_append(GTK_BOX(left), buttons);
    gtk_box_append(GTK_BOX(left), left_label("Recent cases", "heading"));
    app->recent_list = gtk_list_box_new();
    gtk_widget_add_css_class(app->recent_list, "recent");
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app->recent_list), FALSE);
    g_signal_connect(app->recent_list, "row-activated", G_CALLBACK(recent_activated), app);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->recent_list);
    gtk_widget_set_size_request(scroller, -1, 240);
    gtk_widget_add_css_class(scroller, "recent");
    gtk_box_append(GTK_BOX(left), scroller);
    gtk_box_append(GTK_BOX(row), left);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(right, 420, -1);
    gtk_box_append(GTK_BOX(right), left_label("Display filters", "heading"));
    for (int i = 0; i < 5; i++) {
        GtkWidget *link = gtk_button_new_with_label(EXAMPLES[i][0]);
        gtk_widget_add_css_class(link, "link");
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        gtk_actionable_set_action_target(GTK_ACTIONABLE(link), "s", EXAMPLES[i][1]);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(link), "win.filter-apply");
        gtk_box_append(GTK_BOX(right), link);
    }
    gtk_box_append(GTK_BOX(row), right);
    return page;
}

static void entry_activated(GtkEntry *entry, gpointer user) {
    (void)entry;
    app_apply(user, NULL);
}

static gboolean files_dropped(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user) {
    (void)target;
    (void)x;
    (void)y;
    App *app = user;
    GSList *files = g_value_get_boxed(value);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    guint cases = 0;
    for (GSList *item = files; item; item = item->next) {
        char *path = g_file_get_path(item->data);
        if (!path)
            continue;
        gboolean is_case = g_str_has_suffix(path, ".bec") || g_str_has_suffix(path, ".BEC");
        if (!is_case && !import_supported(path)) {
            g_free(path);
            g_ptr_array_free(paths, TRUE);
            return FALSE;
        }
        cases += is_case;
        g_ptr_array_add(paths, path);
    }
    if (paths->len == 1 && cases == 1) {
        app_load(app, paths->pdata[0], FALSE);
        g_ptr_array_free(paths, TRUE);
    } else if (cases) {
        app_alert(app, APP_TITLE, "Open one case, then drop its log files.");
        g_ptr_array_free(paths, TRUE);
    } else if (paths->len && !app_busy(app)) {
        app_open_files_then(app, paths);
    } else {
        g_ptr_array_free(paths, TRUE);
    }
    return TRUE;
}

static gboolean close_requested(GtkWindow *window, gpointer user) {
    App *app = user;
    if (app_busy(app))
        return TRUE;
    cancel_query(app);
    statistics_close_all(app);
    int width, height;
    gtk_window_get_default_size(window, &width, &height);
    g_key_file_set_integer(app->settings, "window", "width", width);
    g_key_file_set_integer(app->settings, "window", "height", height);
    g_key_file_set_integer(app->settings, "window", "mainSplit", gtk_paned_get_position(GTK_PANED(app->split)));
    g_key_file_set_integer(app->settings, "window", "detailSplit",
                           gtk_paned_get_position(GTK_PANED(inspector_widget(app->inspector))));
    settings_save(app);
    timeline_reset(app->timeline, NULL, NULL);
    if (app->db) {
        sqlite3_close(app->db);
        app->db = NULL;
    }
    return FALSE;
}

App *app_new(GtkApplication *gtk) {
    App *app = g_new0(App, 1);
    app->gtk = gtk;
    app->settings = g_key_file_new();
    const char *override = g_getenv("M365_BEC_SETTINGS");
    app->settings_path = override ? g_strdup(override)
                                  : g_build_filename(g_get_user_config_dir(), "M365-BEC", "settings.ini", NULL);
    g_key_file_load_from_file(app->settings, app->settings_path, G_KEY_FILE_NONE, NULL);
    app->logs = g_ptr_array_new_with_free_func(log_free);
    app->views = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, view_free);
    app->closed_tabs = g_array_new(FALSE, FALSE, sizeof(gint64));
    app->history = g_ptr_array_new_with_free_func(g_free);
    app->stats = g_ptr_array_new();
    app->recent_menu = g_menu_new();
    app->log_menu = g_menu_new();
    app->filter_menu = g_menu_new();
    app->zoom = 9;
    app->window = GTK_WINDOW(gtk_application_window_new(gtk));
    gtk_window_set_title(app->window, APP_TITLE);
    gtk_window_set_icon_name(app->window, "m365-bec");
    gtk_window_set_default_size(app->window, g_key_file_get_integer(app->settings, "window", "width", NULL) > 300
                                                 ? g_key_file_get_integer(app->settings, "window", "width", NULL) : 1370,
                                g_key_file_get_integer(app->settings, "window", "height", NULL) > 200
                                    ? g_key_file_get_integer(app->settings, "window", "height", NULL) : 830);
    gtk_widget_set_size_request(GTK_WIDGET(app->window), 720, 440);

    static const char *const plain[] = {"open", "new", "case", "close-case", "case-folder", "clear-recent", "export", "extract",
                                        "close", "quit", "copy", "undo", "home", "all", "reopen", "zoom-in", "zoom-out",
                                        "zoom-reset", "previous", "next", "first", "last", "focus", "find", "clear", "apply",
                                        "save-filter", "saved-filters", "quality", "help"};
    for (guint i = 0; i < G_N_ELEMENTS(plain); i++) {
        GSimpleAction *action = g_simple_action_new(plain[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(activate), app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(action));
        g_object_unref(action);
    }
    static const char *const texts[] = {"recent", "mark-events", "mark-ips", "mark-range", "tab", "stat", "filter-apply",
                                        "filter-prepare", "filter-case", "copy-text", "add-column", "follow-event"};
    for (guint i = 0; i < G_N_ELEMENTS(texts); i++) {
        GSimpleAction *action = g_simple_action_new(texts[i], G_VARIANT_TYPE_STRING);
        g_signal_connect(action, "activate", G_CALLBACK(activate), app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(action));
        g_object_unref(action);
    }
    static const char *const toggles[][2] = {{"pane-details", "detailsPaneVisible"}, {"pane-source", "sourcePaneVisible"},
                                             {"colors", "verdictColors"}, {"stack", "inspectorVertical"}};
    for (guint i = 0; i < G_N_ELEMENTS(toggles); i++) {
        gboolean state = setting_bool(app, toggles[i][1], i != 3);
        GSimpleAction *action = g_simple_action_new_stateful(toggles[i][0], NULL, g_variant_new_boolean(state));
        g_signal_connect(action, "change-state", G_CALLBACK(change_state), app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(action));
        g_object_unref(action);
    }
    static const char *const accels[][3] = {
        {"win.open", "<Control>o"}, {"win.new", "<Control>n"}, {"win.case", "<Control><Shift>o"},
        {"win.export", "<Control><Shift>e"}, {"win.close", "<Control>w"}, {"win.quit", "<Alt>F4"},
        {"win.copy", "<Control><Shift>c"}, {"win.undo", "<Control>z"}, {"win.all", "<Control><Shift>a"},
        {"win.reopen", "<Control><Shift>t"}, {"win.zoom-in", "<Control>plus", "<Control>equal"},
        {"win.zoom-out", "<Control>minus"}, {"win.zoom-reset", "<Control>0"}, {"win.previous", "<Alt>Up"},
        {"win.next", "<Alt>Down"}, {"win.first", "<Control>Home"}, {"win.last", "<Control>End"},
        {"win.focus", "<Control>slash"}, {"win.find", "<Control>f"}, {"win.clear", "<Control><Shift>l"},
        {"win.save-filter", "<Control><Shift>s"}, {"win.stat::activity", "<Control><Shift>i"}, {"win.help", "F1"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(accels); i++)
        gtk_application_set_accels_for_action(gtk, accels[i][0], (const char *const[]){accels[i][1], accels[i][2], NULL});

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(app->window, outer);
    GMenuModel *menus = build_menus(app);
    gtk_box_append(GTK_BOX(outer), gtk_popover_menu_bar_new_from_model(menus));
    g_object_unref(menus);
    build_toolbar(app);
    gtk_box_append(GTK_BOX(outer), app->toolbar);
    GtkWidget *filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(filter_bar, "filterBar");
    GtkWidget *examples = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(examples), glyph(0xE8A4, "\xe2\x89\xa1", "#91b9df"));
    gtk_widget_add_css_class(examples, "tool");
    gtk_widget_set_tooltip_text(examples, "Filter expressions and history");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(examples), G_MENU_MODEL(app->filter_menu));
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(examples), build_filter_menu, app, NULL);
    gtk_box_append(GTK_BOX(filter_bar), examples);
    app->entry = gtk_entry_new();
    gtk_widget_add_css_class(app->entry, "mono");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry), "Apply a BQL display filter \xe2\x80\xa6  <Ctrl+/>");
    gtk_accessible_update_property(GTK_ACCESSIBLE(app->entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "BQL display filter", -1);
    gtk_widget_set_hexpand(app->entry, TRUE);
    g_signal_connect(app->entry, "activate", G_CALLBACK(entry_activated), app);
    g_signal_connect(app->entry, "changed", G_CALLBACK(validate), app);
    gtk_box_append(GTK_BOX(filter_bar), app->entry);
    GtkWidget *apply = glyph_button(0xE72A, "\xe2\x86\x92", "#a5cbed", "Apply display filter");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(apply), "win.apply");
    gtk_box_append(GTK_BOX(filter_bar), apply);
    GtkWidget *clear = glyph_button(0xE711, "\xe2\x9c\x95", "#a5cbed", "Clear display filter");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(clear), "win.clear");
    gtk_box_append(GTK_BOX(filter_bar), clear);
    gtk_box_append(GTK_BOX(outer), filter_bar);

    app->pages = gtk_stack_new();
    gtk_widget_set_vexpand(app->pages, TRUE);
    gtk_box_append(GTK_BOX(outer), app->pages);
    gtk_stack_add_named(GTK_STACK(app->pages), build_welcome(app), "welcome");
    GtkWidget *work = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(app->pages), work, "work");
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    app->tabs = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->tabs), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(app->tabs), FALSE);
    gtk_widget_set_hexpand(app->tabs, TRUE);
    g_signal_connect(app->tabs, "switch-page", G_CALLBACK(page_switched), app);
    gtk_box_append(GTK_BOX(strip), app->tabs);
    GtkWidget *open_tab = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(open_tab), "+");
    gtk_widget_add_css_class(open_tab, "tool");
    gtk_widget_set_tooltip_text(open_tab, "Open log tab");
    gtk_accessible_update_property(GTK_ACCESSIBLE(open_tab), GTK_ACCESSIBLE_PROPERTY_LABEL, "Open log tab", -1);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(open_tab), G_MENU_MODEL(app->log_menu));
    gtk_box_append(GTK_BOX(strip), open_tab);
    gtk_box_append(GTK_BOX(work), strip);
    app->split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_shrink_start_child(GTK_PANED(app->split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->split), FALSE);
    gtk_widget_set_vexpand(app->split, TRUE);
    gtk_box_append(GTK_BOX(work), app->split);
    app->timeline = timeline_new(app);
    gtk_paned_set_start_child(GTK_PANED(app->split), timeline_widget(app->timeline));
    app->inspector = inspector_new(app);
    gtk_paned_set_end_child(GTK_PANED(app->split), inspector_widget(app->inspector));
    gtk_widget_set_size_request(timeline_widget(app->timeline), -1, 90);
    gtk_widget_set_size_request(inspector_widget(app->inspector), -1, 90);
    int main_split = g_key_file_get_integer(app->settings, "window", "mainSplit", NULL);
    gtk_paned_set_position(GTK_PANED(app->split), main_split > 60 ? main_split : 400);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(status, "statusbar");
    app->case_name = left_label("No case open", "caseName");
    gtk_box_append(GTK_BOX(status), app->case_name);
    gtk_box_append(GTK_BOX(status), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    app->status = left_label("Ready to open email logs", NULL);
    gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(app->status, TRUE);
    gtk_box_append(GTK_BOX(status), app->status);
    app->case_saved = left_label("", "secondary");
    gtk_box_append(GTK_BOX(status), app->case_saved);
    app->warnings = gtk_button_new_with_label("");
    gtk_widget_add_css_class(app->warnings, "tool");
    gtk_widget_add_css_class(app->warnings, "warning");
    gtk_widget_set_tooltip_text(app->warnings, "Open import warnings and evidence coverage");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(app->warnings), "win.quality");
    gtk_widget_set_visible(app->warnings, FALSE);
    gtk_box_append(GTK_BOX(status), app->warnings);
    app->scope = gtk_label_new("No case open");
    gtk_box_append(GTK_BOX(status), app->scope);
    gtk_box_append(GTK_BOX(status), gtk_label_new("Time: UTC"));
    gtk_box_append(GTK_BOX(outer), status);

    app->zoom_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(app->zoom_css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(files_dropped), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), GTK_EVENT_CONTROLLER(drop));
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_requested), app);
    g_signal_connect(app->window, "realize", G_CALLBACK(caption_colors), NULL);

    inspector_set_vertical(app->inspector, setting_bool(app, "inspectorVertical", FALSE));
    int detail_split = g_key_file_get_integer(app->settings, "window", "detailSplit", NULL);
    if (detail_split > 60)
        gtk_paned_set_position(GTK_PANED(inspector_widget(app->inspector)), detail_split);
    show_panes(app);
    timeline_set_colors(app->timeline, setting_bool(app, "verdictColors", TRUE));
    show_recent(app);
    update_case_bar(app);
    update_actions(app);
    return app;
}
