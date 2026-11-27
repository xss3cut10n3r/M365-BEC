#include "ui.h"

#include <string.h>

const char *const EXAMPLES[][2] = {
    {"Marked events", "marked = true"},
    {"Failed sign-ins", "source contains \"sign-ins\" AND result = \"Failure\""},
    {"Sent messages", "operation = \"Send\""},
    {"Mailbox access", "operation = \"MailItemsAccessed\" OR operation = \"AttachmentAccess\""},
    {"Inbox rules and forwarding", "operation contains \"InboxRule\" OR operation = \"Set-Mailbox\""},
    {"Consent and permissions",
     "source = \"Entra directory audit\" AND (operation contains \"consent\" OR operation contains \"application\" OR "
     "operation contains \"service principal\" OR operation contains \"app role\" OR operation contains \"permission grant\")"},
    {"Device changes", "source = \"Entra directory audit\" AND operation contains \"device\""},
};
const int EXAMPLE_COUNT = G_N_ELEMENTS(EXAMPLES);

static GtkWidget *two_column_list(GtkListStore **store, const char *first, const char *second) {
    *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(*store));
    const char *titles[] = {first, second};
    for (int i = 0; i < 2; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_min_width(column, i ? 120 : 225);
        gtk_tree_view_column_set_expand(column, i == 1);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    return view;
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), child);
    gtk_widget_set_vexpand(scroller, TRUE);
    return scroller;
}

static GtkWidget *spacer(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(box, TRUE);
    return box;
}

static void close_window(GtkButton *button, gpointer window) {
    (void)button;
    gtk_window_close(GTK_WINDOW(window));
}

static char *selected_text(GtkWidget *view, int column) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *text = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model, &iter))
        gtk_tree_model_get(model, &iter, column, &text, -1);
    return text;
}

/* BQL reference. */

static void use_filter(GtkWidget *view) {
    App *app = g_object_get_data(G_OBJECT(view), "app");
    char *expression = selected_text(view, 1);
    if (!expression)
        return;
    gtk_window_close(GTK_WINDOW(gtk_widget_get_root(view)));
    app_apply(app, expression);
    g_free(expression);
}

static void use_clicked(GtkButton *button, gpointer view) {
    (void)button;
    use_filter(view);
}

static void list_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user) {
    (void)path;
    (void)column;
    (void)user;
    use_filter(GTK_WIDGET(view));
}

static int compare_names(gconstpointer a, gconstpointer b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void dialog_help(App *app) {
    GtkWidget *content;
    GtkWidget *window = dialog_window(app->window, "BQL display filters", 700, 490, &content);
    GtkWidget *notes = gtk_label_new(
        "Compare fields using =, !=, contains, <, <=, > or >=. Combine with AND, OR, NOT and parentheses. Time comparisons "
        "use UTC. Use double quotes around text. For nested fields, try field \"/AuditData/Parameters/*/Name\" = "
        "\"MoveToFolder\". JSON imports use the same paths, such as field \"/status/errorCode\" = \"50126\".");
    gtk_label_set_wrap(GTK_LABEL(notes), TRUE);
    gtk_label_set_xalign(GTK_LABEL(notes), 0);
    gtk_box_append(GTK_BOX(content), notes);
    GPtrArray *names = g_ptr_array_new();
    for (int i = 0; BQL_FIELDS[i]; i++)
        g_ptr_array_add(names, (gpointer)BQL_FIELDS[i]);
    for (int i = 0; i < SLOT_COUNT; i++)
        g_ptr_array_add(names, (gpointer)SLOTS[i].name);
    static const char *const extra[] = {"id", "row", "ip", "mailbox", "marked"};
    for (guint i = 0; i < G_N_ELEMENTS(extra); i++)
        g_ptr_array_add(names, (gpointer)extra[i]);
    g_ptr_array_sort(names, compare_names);
    GString *list = g_string_new("Available fields    ");
    for (guint i = 0; i < names->len; i++)
        g_string_append_printf(list, "%s%s", i ? "  \xc2\xb7  " : "", (char *)names->pdata[i]);
    g_ptr_array_free(names, TRUE);
    GtkWidget *fields = gtk_label_new(list->str);
    g_string_free(list, TRUE);
    gtk_label_set_wrap(GTK_LABEL(fields), TRUE);
    gtk_label_set_xalign(GTK_LABEL(fields), 0);
    gtk_label_set_selectable(GTK_LABEL(fields), TRUE);
    gtk_box_append(GTK_BOX(content), fields);
    GtkListStore *store;
    GtkWidget *examples = two_column_list(&store, "Investigation", "Display filter");
    for (int i = 0; i < EXAMPLE_COUNT; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, EXAMPLES[i][0], 1, EXAMPLES[i][1], -1);
    }
    g_object_set_data(G_OBJECT(examples), "app", app);
    g_signal_connect(examples, "row-activated", G_CALLBACK(list_activated), NULL);
    gtk_box_append(GTK_BOX(content), scrolled(examples));
    GtkWidget *row = button_row(content);
    GtkWidget *use = gtk_button_new_with_label("Use selected filter");
    g_signal_connect(use, "clicked", G_CALLBACK(use_clicked), examples);
    gtk_box_append(GTK_BOX(row), use);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(close_window), window);
    gtk_box_append(GTK_BOX(row), close);
    gtk_window_present(GTK_WINDOW(window));
}

/* Prompts and confirmations. */

typedef struct {
    App *app;
    GtkWidget *window, *entry;
    TextReply text;
    ChoiceReply choice;
    gpointer user;
} Ask;

static void prompt_accept(GtkWidget *widget, gpointer data) {
    (void)widget;
    Ask *ask = data;
    char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(ask->entry)));
    TextReply reply = ask->text;
    App *app = ask->app;
    gpointer user = ask->user;
    gtk_window_close(GTK_WINDOW(ask->window));
    reply(app, text, user);
    g_free(text);
}

static void ask_destroyed(GtkWidget *widget, gpointer data) {
    (void)widget;
    g_free(data);
}

void dialog_prompt(App *app, const char *title, const char *label, TextReply reply, gpointer user) {
    Ask *ask = g_new0(Ask, 1);
    ask->app = app;
    ask->text = reply;
    ask->user = user;
    GtkWidget *content;
    ask->window = dialog_window(app->window, title, 380, -1, &content);
    GtkWidget *caption = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(caption), 0);
    gtk_box_append(GTK_BOX(content), caption);
    ask->entry = gtk_entry_new();
    g_signal_connect(ask->entry, "activate", G_CALLBACK(prompt_accept), ask);
    gtk_box_append(GTK_BOX(content), ask->entry);
    GtkWidget *row = button_row(content);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget *ok = gtk_button_new_with_label("OK");
    g_signal_connect(ok, "clicked", G_CALLBACK(prompt_accept), ask);
    gtk_box_append(GTK_BOX(row), ok);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(close_window), ask->window);
    gtk_box_append(GTK_BOX(row), cancel);
    g_signal_connect(ask->window, "destroy", G_CALLBACK(ask_destroyed), ask);
    gtk_window_present(GTK_WINDOW(ask->window));
}

static void confirm_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    Ask *ask = data;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    ask->choice(ask->app, choice == 0, ask->user);
    g_free(ask);
}

void dialog_confirm(App *app, const char *title, const char *detail, const char *accept, ChoiceReply reply, gpointer user) {
    Ask *ask = g_new0(Ask, 1);
    ask->app = app;
    ask->choice = reply;
    ask->user = user;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_buttons(dialog, (const char *[]){accept, "Cancel", NULL});
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_set_cancel_button(dialog, 1);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_choose(dialog, app->window, NULL, confirm_chosen, ask);
    g_object_unref(dialog);
}

/* Saved display filters. */

GPtrArray *saved_filters(App *app) {
    GPtrArray *filters = g_ptr_array_new_with_free_func(g_free);
    if (!app->db || sql_int(app->db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='display_filters'", NULL) < 1)
        return filters;
    sqlite3_stmt *rows = sql_prepare(app->db, "SELECT name,expression FROM display_filters ORDER BY name COLLATE NOCASE", NULL);
    while (rows && sqlite3_step(rows) == SQLITE_ROW) {
        g_ptr_array_add(filters, g_strdup(column_text(rows, 0)));
        g_ptr_array_add(filters, g_strdup(column_text(rows, 1)));
    }
    sqlite3_finalize(rows);
    return filters;
}

static void store_filter(App *app, const char *name, const char *expression) {
    GError *error = NULL;
    sql_run(app->db, "CREATE TABLE IF NOT EXISTS display_filters(name TEXT PRIMARY KEY COLLATE NOCASE,expression TEXT NOT NULL)",
            &error);
    sqlite3_stmt *insert = error ? NULL : sql_prepare(app->db, "INSERT OR REPLACE INTO display_filters VALUES(?,?)", &error);
    if (insert) {
        sqlite3_bind_text(insert, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, expression, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE)
            bec_fail(&error, "%s", sqlite3_errmsg(app->db));
        sqlite3_finalize(insert);
    }
    if (error) {
        app_alert(app, "Save filter", error->message);
        g_clear_error(&error);
        return;
    }
    char *message = g_strdup_printf("Saved display filter \xc2\xb7 %s", name);
    app_message(app, message);
    app_saved(app, "display filter");
    g_free(message);
}

static void replace_confirmed(App *app, gboolean accepted, gpointer user) {
    char **pair = user;
    if (accepted)
        store_filter(app, pair[0], pair[1]);
    g_strfreev(pair);
}

static void name_given(App *app, const char *text, gpointer user) {
    char *expression = user;
    char *name = py_strip(text);
    if (*name && app->db) {
        GPtrArray *existing = saved_filters(app);
        char *folded = casefold(name);
        gboolean clash = FALSE;
        for (guint i = 0; i + 1 < existing->len && !clash; i += 2)
            clash = equals_folded(existing->pdata[i], folded);
        g_free(folded);
        g_ptr_array_free(existing, TRUE);
        if (clash) {
            char **pair = g_new0(char *, 3);
            pair[0] = g_strdup(name);
            pair[1] = g_strdup(expression);
            dialog_confirm(app, "Save display filter", "Replace the existing named filter?", "Replace", replace_confirmed, pair);
        } else {
            store_filter(app, name, expression);
        }
    }
    g_free(name);
    g_free(expression);
}

void dialog_save_filter(App *app) {
    if (!app->db || app_busy(app))
        return;
    char *expression = py_strip(gtk_editable_get_text(GTK_EDITABLE(app->entry)));
    if (!*expression) {
        app_alert(app, APP_TITLE, "Enter a display filter first.");
        g_free(expression);
        return;
    }
    GError *error = NULL;
    Query *query = query_new(expression, &error);
    if (!query) {
        app_alert(app, "Save filter", error->message);
        g_clear_error(&error);
        g_free(expression);
        return;
    }
    query_free(query);
    dialog_prompt(app, "Save display filter", "Name", name_given, expression);
}

static void delete_confirmed(App *app, gboolean accepted, gpointer user) {
    GtkWidget *view = user;
    char *name = accepted ? selected_text(view, 0) : NULL;
    if (name && !app_busy(app)) {
        sqlite3_stmt *remove = sql_prepare(app->db, "DELETE FROM display_filters WHERE name=?", NULL);
        if (remove) {
            sqlite3_bind_text(remove, 1, name, -1, SQLITE_TRANSIENT);
            sqlite3_step(remove);
            sqlite3_finalize(remove);
        }
        GtkTreeModel *model;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model, &iter))
            gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
        app_saved(app, "display filters");
    }
    g_free(name);
}

static void delete_clicked(GtkButton *button, gpointer view) {
    (void)button;
    App *app = g_object_get_data(G_OBJECT(view), "app");
    char *name = selected_text(view, 0);
    if (!name)
        return;
    char *question = g_strdup_printf("Delete %s?", name);
    dialog_confirm(app, "Delete display filter", question, "Delete", delete_confirmed, view);
    g_free(question);
    g_free(name);
}

void dialog_saved_filters(App *app) {
    if (!app->db)
        return;
    GtkWidget *content;
    GtkWidget *window = dialog_window(app->window, "Saved display filters", 680, 330, &content);
    GtkListStore *store;
    GtkWidget *view = two_column_list(&store, "Name", "BQL expression");
    GPtrArray *filters = saved_filters(app);
    for (guint i = 0; i + 1 < filters->len; i += 2) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, filters->pdata[i], 1, filters->pdata[i + 1], -1);
    }
    g_ptr_array_free(filters, TRUE);
    g_object_set_data(G_OBJECT(view), "app", app);
    g_signal_connect(view, "row-activated", G_CALLBACK(list_activated), NULL);
    gtk_box_append(GTK_BOX(content), scrolled(view));
    GtkWidget *row = button_row(content);
    GtkWidget *apply = gtk_button_new_with_label("Apply");
    g_signal_connect(apply, "clicked", G_CALLBACK(use_clicked), view);
    gtk_box_append(GTK_BOX(row), apply);
    GtkWidget *remove = gtk_button_new_with_label("Delete");
    g_signal_connect(remove, "clicked", G_CALLBACK(delete_clicked), view);
    gtk_box_append(GTK_BOX(row), remove);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(close_window), window);
    gtk_box_append(GTK_BOX(row), close);
    gtk_window_present(GTK_WINDOW(window));
}

/* Evidence and import details. */

typedef struct {
    GtkListStore *store;
    GtkWidget *message, *window;
    GCancellable *cancel;
    char *path;
    GPtrArray *rows;
    GPtrArray *evidence;
    char *error;
} Details;

static int stop_requested(void *cancel) {
    return g_cancellable_is_cancelled(cancel);
}

static gboolean details_done(gpointer data) {
    Details *details = data;
    if (!g_cancellable_is_cancelled(details->cancel)) {
        gint64 total = 0;
        for (guint i = 0; details->rows && i < details->rows->len; i++) {
            QualityRow *row = details->rows->pdata[i];
            total += row->total;
            char *name = *row->sheet && !log_is_json(row->name) ? g_strdup_printf("%s / %s", row->name, row->sheet) : g_strdup(row->name);
            char *records = group_digits(row->total), *issues = g_strdup_printf("%" G_GINT64_FORMAT, row->issues);
            char *linked = row->linked < 0 ? g_strdup("-") : group_digits(row->linked);
            char *missing = row->missing < 0 ? g_strdup("-") : group_digits(row->missing);
            GtkTreeIter iter;
            gtk_list_store_append(details->store, &iter);
            gtk_list_store_set(details->store, &iter, 0, name, 1, row->kind, 2, records, 3, issues, 4, linked, 5, missing, 6,
                               *row->warning ? row->warning : "-", 7,
                               i < details->evidence->len ? (char *)details->evidence->pdata[i] : "", -1);
            g_free(name);
            g_free(records);
            g_free(issues);
            g_free(linked);
            g_free(missing);
        }
        if (details->error) {
            gtk_label_set_text(GTK_LABEL(details->message), details->error);
        } else {
            char *records = group_digits(total);
            char *text = g_strdup_printf("%u log(s) \xc2\xb7 %s source records \xc2\xb7 archived copies verified against SHA-256",
                                         details->rows ? details->rows->len : 0, records);
            gtk_label_set_text(GTK_LABEL(details->message), text);
            g_free(text);
            g_free(records);
        }
    }
    if (details->rows)
        g_ptr_array_free(details->rows, TRUE);
    g_ptr_array_free(details->evidence, TRUE);
    g_object_unref(details->cancel);
    g_free(details->path);
    g_free(details->error);
    g_free(details);
    return G_SOURCE_REMOVE;
}

#include <zlib.h>

// Recheck archived bytes against hashes.
static char *verify_file(sqlite3 *db, gint64 file, const char *digest, GCancellable *cancel) {
    sqlite3_stmt *chunks = sql_prepare(db, "SELECT data FROM chunks WHERE file=? ORDER BY part", NULL);
    if (!chunks)
        return g_strdup("Unreadable");
    sqlite3_bind_int64(chunks, 1, file);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize room = 2 * 1024 * 1024;
    guchar *buffer = g_malloc(room);
    gboolean ok = TRUE;
    while (ok && sqlite3_step(chunks) == SQLITE_ROW && !g_cancellable_is_cancelled(cancel)) {
        uLongf size = (uLongf)room;
        ok = uncompress(buffer, &size, sqlite3_column_blob(chunks, 0), (uLong)sqlite3_column_bytes(chunks, 0)) == Z_OK;
        if (ok)
            g_checksum_update(sum, buffer, (gssize)size);
    }
    sqlite3_finalize(chunks);
    char *verdict = g_strdup(ok && !strcmp(g_checksum_get_string(sum), digest) ? "Intact" : "MISMATCH");
    g_checksum_free(sum);
    g_free(buffer);
    return verdict;
}

static gpointer details_run(gpointer data) {
    Details *details = data;
    GError *error = NULL;
    sqlite3 *db = case_read(details->path, &error);
    if (db) {
        sqlite3_progress_handler(db, 1000, stop_requested, details->cancel);
        details->rows = stats_quality(db, &error);
        GHashTable *checked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
        sqlite3_stmt *files = sql_prepare(db, "SELECT l.file,f.digest FROM logs l JOIN files f ON f.id=l.file ORDER BY l.id", NULL);
        while (files && sqlite3_step(files) == SQLITE_ROW) {
            gint64 file = sqlite3_column_int64(files, 0);
            char *state = g_hash_table_lookup(checked, &file);
            if (!state) {
                state = verify_file(db, file, column_text(files, 1), details->cancel);
                gint64 *key = g_new(gint64, 1);
                *key = file;
                g_hash_table_insert(checked, key, state);
            }
            g_ptr_array_add(details->evidence, g_strdup(state));
        }
        sqlite3_finalize(files);
        g_hash_table_destroy(checked);
        sqlite3_close(db);
    }
    if (error) {
        details->error = g_strdup(error->message);
        g_clear_error(&error);
    }
    g_idle_add(details_done, details);
    return NULL;
}

static gboolean details_closed(GtkWindow *window, gpointer cancel) {
    (void)window;
    g_cancellable_cancel(cancel);
    g_object_unref(cancel);
    return FALSE;
}

void dialog_import_details(App *app) {
    if (!app->db)
        return;
    GtkWidget *content;
    GtkWidget *window = dialog_window(app->window, "Evidence and import details", 1120, 390, &content);
    GtkListStore *store = gtk_list_store_new(8, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    static const char *const titles[] = {"Source file / sheet", "Log type",        "Records",      "Row issues",
                                         "Linked steps",        "Unmatched steps", "Export limit", "Archived copy"};
    for (int i = 0; i < 8; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", i == 0 || i == 6 ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_expand(column, i == 0 || i == 6);
        if (i == 0 || i == 6)
            gtk_tree_view_column_set_min_width(column, 200);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), 6);
    gtk_box_append(GTK_BOX(content), scrolled(view));
    GtkWidget *message = gtk_label_new("Checking imported files and authentication companions\xe2\x80\xa6");
    gtk_label_set_xalign(GTK_LABEL(message), 0);
    gtk_box_append(GTK_BOX(content), message);
    GtkWidget *row = button_row(content);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(close_window), window);
    gtk_box_append(GTK_BOX(row), close);
    Details *details = g_new0(Details, 1);
    details->store = store;
    details->message = message;
    details->window = window;
    details->cancel = g_cancellable_new();
    details->path = g_strdup(app->path);
    details->evidence = g_ptr_array_new_with_free_func(g_free);
    g_signal_connect(window, "close-request", G_CALLBACK(details_closed), g_object_ref(details->cancel));
    g_thread_unref(g_thread_new("details", details_run, details));
    gtk_window_present(GTK_WINDOW(window));
}

/* Time range verdicts. */

typedef struct {
    App *app;
    GtkWidget *window, *start, *end, *message;
    char *verdict;
} Range;

static void range_accept(GtkWidget *widget, gpointer data) {
    (void)widget;
    Range *range = data;
    char *lo = utc_normalize(gtk_editable_get_text(GTK_EDITABLE(range->start)));
    char *hi = utc_normalize(gtk_editable_get_text(GTK_EDITABLE(range->end)));
    if (!*lo || !*hi || strcmp(hi, lo) <= 0) {
        gtk_label_set_text(GTK_LABEL(range->message), "Enter an end time after the start.");
    } else {
        App *app = range->app;
        GPtrArray *pairs = g_ptr_array_new();
        g_ptr_array_add(pairs, lo);
        g_ptr_array_add(pairs, hi);
        GError *error = NULL;
        if (!assign_verdict(app->db, "time", pairs, range->verdict, &error)) {
            gtk_label_set_text(GTK_LABEL(range->message), error->message);
            g_clear_error(&error);
        } else {
            gtk_window_close(GTK_WINDOW(range->window));
            app_saved(app, "time range verdict");
            statistics_refresh_all(app);
            app_refresh(app);
        }
        g_ptr_array_free(pairs, TRUE);
    }
    g_free(lo);
    g_free(hi);
}

static void range_destroyed(GtkWidget *widget, gpointer data) {
    (void)widget;
    Range *range = data;
    g_free(range->verdict);
    g_free(range);
}

void dialog_mark_time(App *app, const char *verdict) {
    if (!app->has_active || app_busy(app))
        return;
    // Cached records may be evicted.
    char *first = NULL, *last = NULL;
    GArray *selected = timeline_selected(app->timeline);
    for (guint i = 0; i < selected->len; i++) {
        Record *record = timeline_record(app->timeline, g_array_index(selected, guint, i));
        if (!record || !*record->time)
            continue;
        if (!first || strcmp(record->time, first) < 0) {
            g_free(first);
            first = g_strdup(record->time);
        }
        if (!last || strcmp(record->time, last) > 0) {
            g_free(last);
            last = g_strdup(record->time);
        }
    }
    Range *range = g_new0(Range, 1);
    range->app = app;
    range->verdict = g_strdup(verdict);
    GtkWidget *content;
    char *title = g_strdup_printf("Mark UTC time range \xc2\xb7 %s", verdict);
    range->window = dialog_window(app->window, title, 430, -1, &content);
    g_free(title);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    range->start = gtk_entry_new();
    range->end = gtk_entry_new();
    gtk_widget_set_hexpand(range->start, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(range->start), first ? first : "");
    if (last && !g_str_has_prefix(last, "9999-12-31")) {
        char *after = utc_after(last);
        gtk_editable_set_text(GTK_EDITABLE(range->end), after);
        g_free(after);
    }
    GtkWidget *from = gtk_label_new("From UTC (inclusive)"), *until = gtk_label_new("Until UTC (exclusive)");
    gtk_label_set_xalign(GTK_LABEL(from), 0);
    gtk_label_set_xalign(GTK_LABEL(until), 0);
    gtk_grid_attach(GTK_GRID(grid), from, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), range->start, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), until, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), range->end, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(content), grid);
    range->message = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(range->message), 0);
    gtk_box_append(GTK_BOX(content), range->message);
    GtkWidget *row = button_row(content);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget *ok = gtk_button_new_with_label("OK");
    g_signal_connect(ok, "clicked", G_CALLBACK(range_accept), range);
    g_signal_connect(range->end, "activate", G_CALLBACK(range_accept), range);
    gtk_box_append(GTK_BOX(row), ok);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(close_window), range->window);
    gtk_box_append(GTK_BOX(row), cancel);
    g_signal_connect(range->window, "destroy", G_CALLBACK(range_destroyed), range);
    g_array_free(selected, TRUE);
    g_free(first);
    g_free(last);
    gtk_window_present(GTK_WINDOW(range->window));
}
