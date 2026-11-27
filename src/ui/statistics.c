#include "ui.h"

#include <glib/gstdio.h>
#include <string.h>

enum { S_A, S_B, S_C, S_EVENTS, S_PERCENT, S_FIRST, S_LAST, S_INDEX, S_EPOCH, S_COUNT };

typedef struct {
    App *app;
    GtkWidget *window, *kind, *interval, *search, *scope, *limit, *filter_label, *plot, *table, *message, *apply;
    GtkListStore *store;
    GtkTreeModel *filtered, *sorted;
    char *path, *original_filter, *base, *key;
    gint64 original_log, log, total;
    guint generation;
    GCancellable *cancel;
    GPtrArray *rows;
    int step, width;
    gboolean closed;
    double hover_x;
} Stats;

typedef struct {
    Stats *stats;
    guint generation;
    GCancellable *cancel;
    char *path, *key, *filter;
    gint64 log, total;
    int step;
    GPtrArray *rows;
    char *error;
} Job;

static const int STEPS[] = {60, 900, 3600, 86400};

static int stop_requested(void *cancel) {
    return g_cancellable_is_cancelled(cancel);
}

static gboolean job_done(gpointer data) {
    Job *job = data;
    Stats *stats = job->stats;
    if (!g_cancellable_is_cancelled(job->cancel) && !stats->closed && job->generation == stats->generation) {
        const Report *report = report_find(job->key);
        gboolean activity = !strcmp(job->key, "activity");
        if (stats->rows)
            g_ptr_array_free(stats->rows, TRUE);
        stats->rows = job->rows ? job->rows : g_ptr_array_new_with_free_func((GDestroyNotify)stat_row_free);
        job->rows = NULL;
        stats->total = job->total;
        g_free(stats->key);
        stats->key = g_strdup(job->key);
        stats->step = job->step;
        stats->width = report_width(report);
        // Rebuild columns for this report.
        gtk_list_store_clear(stats->store);
        while (gtk_tree_view_get_n_columns(GTK_TREE_VIEW(stats->table)))
            gtk_tree_view_remove_column(GTK_TREE_VIEW(stats->table), gtk_tree_view_get_column(GTK_TREE_VIEW(stats->table), 0));
        const char *tail[] = {"Events", "% displayed", "First seen (UTC)", "Last seen (UTC)"};
        for (int i = 0; i < stats->width + 4; i++) {
            gboolean group = i < stats->width;
            int column_id = group ? S_A + i : S_EVENTS + (i - stats->width);
            GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", !group && i - stats->width < 2 ? 1.0 : 0.0, NULL);
            GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
                group ? report->headings[i] : tail[i - stats->width], renderer, "text",
                column_id == S_EVENTS ? S_COUNT : column_id == S_PERCENT ? S_COUNT + 1 : column_id, NULL);
            gtk_tree_view_column_set_resizable(column, TRUE);
            gtk_tree_view_column_set_expand(column, group);
            gtk_tree_view_column_set_min_width(column, group ? 120 : i - stats->width < 2 ? 86 : 214);
            gtk_tree_view_column_set_sort_column_id(column, activity && group ? S_EPOCH : column_id);
            gtk_tree_view_append_column(GTK_TREE_VIEW(stats->table), column);
        }
        for (guint i = 0; i < stats->rows->len; i++) {
            StatRow *row = stats->rows->pdata[i];
            double percent = stats->total ? 100.0 * (double)row->count / (double)stats->total : 0;
            char *events = group_digits(row->count), *share = g_strdup_printf("%.2f%%", percent);
            char *stamp = activity ? utc_from_epoch(row->epoch) : NULL;
            if (stamp)
                stamp[10] = ' ';
            GtkTreeIter iter;
            gtk_list_store_append(stats->store, &iter);
            gtk_list_store_set(stats->store, &iter, S_A, activity ? stamp : row->values[0], S_B, row->values[1], S_C,
                               row->values[2], S_EVENTS, row->count, S_PERCENT, percent, S_FIRST, row->first ? row->first : "",
                               S_LAST, row->last ? row->last : "", S_INDEX, (int)i, S_EPOCH, row->epoch, S_COUNT, events,
                               S_COUNT + 1, share, -1);
            g_free(stamp);
            g_free(events);
            g_free(share);
        }
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(stats->sorted), activity ? S_EPOCH : S_EVENTS,
                                             activity ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
        if (job->error) {
            gtk_label_set_text(GTK_LABEL(stats->message), job->error);
        } else {
            char *groups = group_digits(stats->rows->len), *total = group_digits(stats->total);
            char *text = g_strdup_printf("%s groups \xc2\xb7 %s displayed events", groups, total);
            gtk_label_set_text(GTK_LABEL(stats->message), text);
            g_free(text);
            g_free(groups);
            g_free(total);
        }
        gtk_widget_set_tooltip_text(stats->message,
                                    "Each event contributes once per distinct matching group. Groups can overlap.");
        gtk_widget_set_sensitive(stats->apply, stats->rows->len > 0);
        if (stats->rows->len) {
            GtkTreePath *first = gtk_tree_path_new_first();
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(stats->table), first, NULL, FALSE);
            gtk_tree_path_free(first);
        }
        gtk_widget_queue_draw(stats->plot);
    }
    if (job->rows)
        g_ptr_array_free(job->rows, TRUE);
    g_object_unref(job->cancel);
    g_free(job->path);
    g_free(job->key);
    g_free(job->filter);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer job_run(gpointer data) {
    Job *job = data;
    GError *error = NULL;
    sqlite3 *db = case_read(job->path, &error);
    if (db) {
        sqlite3_progress_handler(db, 1000, stop_requested, job->cancel);
        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        job->total = count_rows(db, job->log, job->filter, &error);
        if (!error)
            job->rows = stats_calculate(db, job->key, job->log, job->filter, job->step, &error);
        sqlite3_close(db);
    }
    if (error) {
        job->error = g_strdup(error->message);
        job->total = 0;
        g_clear_error(&error);
    }
    g_idle_add(job_done, job);
    return NULL;
}

static const char *current_key(Stats *stats) {
    return REPORTS[gtk_drop_down_get_selected(GTK_DROP_DOWN(stats->kind))].key;
}

static void refresh(Stats *stats) {
    if (stats->closed)
        return;
    if (stats->cancel) {
        g_cancellable_cancel(stats->cancel);
        g_object_unref(stats->cancel);
    }
    stats->cancel = g_cancellable_new();
    stats->generation++;
    const char *key = current_key(stats);
    gboolean activity = !strcmp(key, "activity");
    int step = STEPS[gtk_drop_down_get_selected(GTK_DROP_DOWN(stats->interval))];
    gboolean whole_case = !stats->original_log || gtk_drop_down_get_selected(GTK_DROP_DOWN(stats->scope)) == 1;
    stats->log = whole_case ? 0 : stats->original_log;
    g_free(stats->base);
    stats->base = g_strdup(gtk_check_button_get_active(GTK_CHECK_BUTTON(stats->limit)) ? stats->original_filter : "");
    gtk_label_set_text(GTK_LABEL(stats->filter_label), stats->base);
    gtk_widget_set_visible(stats->interval, activity);
    gtk_widget_set_visible(stats->plot, activity);
    gtk_list_store_clear(stats->store);
    if (stats->rows)
        g_ptr_array_set_size(stats->rows, 0);
    gtk_widget_queue_draw(stats->plot);
    gtk_label_set_text(GTK_LABEL(stats->message), "Calculating across matching events\xe2\x80\xa6");
    gtk_widget_set_sensitive(stats->apply, FALSE);
    Job *job = g_new0(Job, 1);
    job->stats = stats;
    job->generation = stats->generation;
    job->cancel = g_object_ref(stats->cancel);
    job->path = g_strdup(stats->path);
    job->key = g_strdup(key);
    job->filter = g_strdup(stats->base);
    job->log = stats->log;
    job->step = step;
    g_thread_unref(g_thread_new("statistics", job_run, job));
}

static void control_changed(GObject *object, GParamSpec *spec, gpointer user) {
    (void)object;
    (void)spec;
    refresh(user);
}

static void limit_toggled(GtkCheckButton *button, gpointer user) {
    (void)button;
    refresh(user);
}

static StatRow *selected_row(Stats *stats) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(stats->table)), &model, &iter))
        return NULL;
    int index;
    gtk_tree_model_get(model, &iter, S_INDEX, &index, -1);
    return stats->rows && (guint)index < stats->rows->len ? stats->rows->pdata[index] : NULL;
}

static void apply_row(Stats *stats, const StatRow *row) {
    char *drill = stats_drilldown(stats->key, row, stats->step);
    char *combined = combine_filters(stats->base, drill);
    App *app = stats->app;
    gint64 log = stats->log;
    gtk_window_close(GTK_WINDOW(stats->window));
    app_apply_in(app, log, combined);
    g_free(combined);
    g_free(drill);
}

static void apply_selected(Stats *stats) {
    StatRow *row = selected_row(stats);
    if (row)
        apply_row(stats, row);
}

static void apply_clicked(GtkButton *button, gpointer user) {
    (void)button;
    apply_selected(user);
}

static void row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user) {
    (void)view;
    (void)path;
    (void)column;
    apply_selected(user);
}

/* Activity plot. */

static gboolean plot_axis(Stats *stats, int width, double *x0, double *w, gint64 *start, gint64 *end) {
    if (!stats->rows || !stats->rows->len || !stats->key || strcmp(stats->key, "activity"))
        return FALSE;
    *x0 = 57;
    *w = MAX(1, width - 57 - 20);
    *start = ((StatRow *)stats->rows->pdata[0])->epoch;
    *end = ((StatRow *)stats->rows->pdata[stats->rows->len - 1])->epoch + stats->step;
    return TRUE;
}

static void plot_text(cairo_t *cr, GtkWidget *widget, const char *text, double x, double y, double width, int align) {
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double left = align == 0 ? x : align == 1 ? x + width - tw : x + (width - tw) / 2;
    cairo_move_to(cr, left, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void plot_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user) {
    Stats *stats = user;
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_set_source_rgb(cr, 0x24 / 255.0, 0x27 / 255.0, 0x29 / 255.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0xc2 / 255.0, 0xcb / 255.0, 0xcc / 255.0);
    plot_text(cr, widget, "Events", 8, 11, 0, 0);
    double x0, w;
    gint64 start, end;
    if (!plot_axis(stats, width, &x0, &w, &start, &end)) {
        plot_text(cr, widget, "No events in this view", 0, height / 2.0, width, 2);
        return;
    }
    double y0 = 22, h = MAX(1, height - y0 - 28);
    gint64 peak = 1;
    for (guint i = 0; i < stats->rows->len; i++)
        peak = MAX(peak, ((StatRow *)stats->rows->pdata[i])->count);
    for (int i = 0; i < 3; i++) {
        double fraction = i * 0.5, y = y0 + (int)(h * (1 - fraction));
        cairo_set_source_rgb(cr, 0x42 / 255.0, 0x49 / 255.0, 0x4b / 255.0);
        cairo_rectangle(cr, x0, y, w, 1);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0xb7 / 255.0, 0xc3 / 255.0, 0xc4 / 255.0);
        char *label = group_digits((gint64)((double)peak * fraction));
        plot_text(cr, widget, label, 1, y, x0 - 7, 1);
        g_free(label);
    }
    cairo_set_source_rgb(cr, 0x8b / 255.0, 0xbb / 255.0, 0xd2 / 255.0);
    for (guint i = 0; i < stats->rows->len; i++) {
        StatRow *row = stats->rows->pdata[i];
        double x = x0 + (int)((double)(row->epoch - start) * w / (double)(end - start));
        double bar = MAX(1, (double)stats->step * w / (double)(end - start) - 1);
        double tall = MAX(1, (double)row->count * h / (double)peak);
        cairo_rectangle(cr, x, y0 + h - tall, bar, tall);
    }
    cairo_fill(cr);
    char *first = utc_from_epoch(start), *last = utc_from_epoch(end);
    first[16] = last[16] = 0;
    first[10] = last[10] = ' ';
    char *last_label = g_strconcat(last, " UTC", NULL);
    cairo_set_source_rgb(cr, 0xc2 / 255.0, 0xcb / 255.0, 0xcc / 255.0);
    plot_text(cr, widget, first, x0, height - 13, 0, 0);
    plot_text(cr, widget, last_label, x0, height - 13, w, 1);
    g_free(last_label);
    g_free(first);
    g_free(last);
}

static gboolean plot_bucket(Stats *stats, double x, gint64 *bucket) {
    double x0, w;
    gint64 start, end;
    if (!plot_axis(stats, gtk_widget_get_width(stats->plot), &x0, &w, &start, &end) || x < x0 || x >= x0 + w)
        return FALSE;
    *bucket = start + (gint64)((x - x0) / w * (double)(end - start) / stats->step) * stats->step;
    return TRUE;
}

static gboolean plot_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard, GtkTooltip *tooltip, gpointer user) {
    (void)widget;
    (void)y;
    (void)keyboard;
    Stats *stats = user;
    gint64 bucket;
    if (!plot_bucket(stats, x, &bucket))
        return FALSE;
    gint64 count = 0;
    for (guint i = 0; i < stats->rows->len; i++)
        if (((StatRow *)stats->rows->pdata[i])->epoch == bucket)
            count = ((StatRow *)stats->rows->pdata[i])->count;
    char *stamp = utc_from_epoch(bucket), *events = group_digits(count);
    char *text = g_strdup_printf("%s \xc2\xb7 %s events", stamp, events);
    gtk_tooltip_set_text(tooltip, text);
    g_free(text);
    g_free(stamp);
    g_free(events);
    return TRUE;
}

static void plot_pressed(GtkGestureClick *gesture, int presses, double x, double y, gpointer user) {
    (void)gesture;
    (void)y;
    Stats *stats = user;
    gint64 bucket;
    if (presses == 2 && plot_bucket(stats, x, &bucket)) {
        StatRow row = {0};
        row.epoch = bucket;
        apply_row(stats, &row);
    }
}

static gboolean row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer user) {
    Stats *stats = user;
    const char *wanted = gtk_editable_get_text(GTK_EDITABLE(stats->search));
    if (!*wanted)
        return TRUE;
    char *needle = casefold(wanted);
    gboolean hit = FALSE;
    static const int columns[] = {S_A, S_B, S_C, S_COUNT, S_COUNT + 1, S_FIRST, S_LAST};
    for (guint i = 0; i < G_N_ELEMENTS(columns) && !hit; i++) {
        char *text = NULL;
        gtk_tree_model_get(model, iter, columns[i], &text, -1);
        hit = text && contains_folded(text, needle);
        g_free(text);
    }
    g_free(needle);
    return hit;
}

static void search_changed(GtkEditable *editable, gpointer user) {
    (void)editable;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(((Stats *)user)->filtered));
}

static char *row_cells(Stats *stats, const StatRow *row, GPtrArray *cells) {
    gboolean activity = !strcmp(stats->key, "activity");
    for (int i = 0; i < stats->width; i++)
        g_ptr_array_add(cells, activity ? utc_from_epoch(row->epoch) : g_strdup(row->values[i]));
    g_ptr_array_add(cells, g_strdup_printf("%" G_GINT64_FORMAT, row->count));
    g_ptr_array_add(cells, py_float_repr(stats->total ? 100.0 * (double)row->count / (double)stats->total : 0));
    g_ptr_array_add(cells, g_strdup(row->first ? row->first : ""));
    g_ptr_array_add(cells, g_strdup(row->last ? row->last : ""));
    return NULL;
}

static void copy_group(GSimpleAction *action, GVariant *parameter, gpointer user) {
    (void)parameter;
    Stats *stats = user;
    StatRow *row = selected_row(stats);
    if (!row)
        return;
    if (!strcmp(g_action_get_name(G_ACTION(action)), "apply")) {
        apply_row(stats, row);
        return;
    }
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    row_cells(stats, row, cells);
    g_ptr_array_remove_index(cells, (guint)stats->width + 1);
    g_ptr_array_add(cells, NULL);
    char *text = g_strjoinv("\t", (char **)cells->pdata);
    copy_text(stats->window, text);
    g_free(text);
    g_ptr_array_free(cells, TRUE);
}

static void table_context(GtkGestureClick *gesture, int presses, double x, double y, gpointer user) {
    (void)presses;
    Stats *stats = user;
    GtkTreePath *path = NULL;
    int bx, by;
    gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(stats->table), (int)x, (int)y, &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(stats->table), bx, by, &path, NULL, NULL, NULL))
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(stats->table), path, NULL, FALSE);
    gtk_tree_path_free(path);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Apply as filter", "stats.apply");
    g_menu_append(menu, "Copy selected group", "stats.copy");
    popup_menu(stats->table, menu, x, y);
    g_object_unref(menu);
}

static void save_chosen(GObject *source, GAsyncResult *result, gpointer user) {
    Stats *stats = user;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file || stats->closed || !stats->rows || !stats->key)
        return;
    char *path = g_file_get_path(file);
    g_object_unref(file);
    GError *error = NULL;
    sqlite3 *db = case_read(stats->path, &error);
    char *temporary = NULL;
    if (db && guard_destination(db, path, &error) && (temporary = temporary_beside(path, &error))) {
        GString *out = g_string_new("\xef\xbb\xbf");
        GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
        const Report *report = report_find(stats->key);
        for (int i = 0; i < stats->width; i++)
            g_ptr_array_add(cells, g_strdup(report->headings[i]));
        static const char *const tail[] = {"Events", "% displayed", "First seen (UTC)", "Last seen (UTC)"};
        for (int i = 0; i < 4; i++)
            g_ptr_array_add(cells, g_strdup(tail[i]));
        char *line = csv_line(cells, FALSE);
        g_string_append(out, line);
        g_free(line);
        GtkTreeIter iter;
        // Save the displayed order.
        if (gtk_tree_model_get_iter_first(stats->sorted, &iter)) {
            do {
                int index;
                gtk_tree_model_get(stats->sorted, &iter, S_INDEX, &index, -1);
                g_ptr_array_set_size(cells, 0);
                row_cells(stats, stats->rows->pdata[index], cells);
                line = csv_line(cells, TRUE);
                g_string_append(out, line);
                g_free(line);
            } while (gtk_tree_model_iter_next(stats->sorted, &iter));
        }
        if (g_file_set_contents(temporary, out->str, (gssize)out->len, &error))
            replace_file(temporary, path, &error);
        g_ptr_array_free(cells, TRUE);
        g_string_free(out, TRUE);
    }
    if (error) {
        if (temporary)
            g_unlink(temporary);
        app_alert(stats->app, "Save statistics", error->message);
        g_clear_error(&error);
    }
    if (db)
        sqlite3_close(db);
    g_free(temporary);
    g_free(path);
}

static void save_clicked(GtkButton *button, gpointer user) {
    (void)button;
    Stats *stats = user;
    if (!stats->rows || !stats->rows->len)
        return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save statistics");
    char *name = g_strconcat(current_key(stats), ".csv", NULL);
    gtk_file_dialog_set_initial_name(dialog, name);
    g_free(name);
    gtk_file_dialog_save(dialog, GTK_WINDOW(stats->window), NULL, save_chosen, stats);
    g_object_unref(dialog);
}

static void close_clicked(GtkButton *button, gpointer user) {
    (void)button;
    gtk_window_close(GTK_WINDOW(((Stats *)user)->window));
}

static gboolean window_closed(GtkWindow *window, gpointer user) {
    (void)window;
    Stats *stats = user;
    stats->closed = TRUE;
    if (stats->cancel)
        g_cancellable_cancel(stats->cancel);
    g_ptr_array_remove(stats->app->stats, stats->window);
    return FALSE;
}

void statistics_close_all(App *app) {
    while (app->stats->len)
        gtk_window_close(GTK_WINDOW(app->stats->pdata[app->stats->len - 1]));
}

void statistics_open(App *app, const char *key) {
    if (!app->has_active)
        return;
    Stats *stats = g_new0(Stats, 1);
    stats->app = app;
    stats->path = g_strdup(app->path);
    stats->original_log = app->active;
    stats->original_filter = g_strdup(app_view(app)->query);
    GtkWidget *content;
    stats->window = dialog_window(app->window, "Statistics", 910, 570, &content);
    gtk_window_set_modal(GTK_WINDOW(stats->window), FALSE);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkStringList *titles = gtk_string_list_new(NULL);
    guint chosen = 0;
    for (int i = 0; i < REPORT_COUNT; i++) {
        gtk_string_list_append(titles, REPORTS[i].title);
        if (!strcmp(REPORTS[i].key, key))
            chosen = (guint)i;
    }
    stats->kind = gtk_drop_down_new(G_LIST_MODEL(titles), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(stats->kind), chosen);
    gtk_accessible_update_property(GTK_ACCESSIBLE(stats->kind), GTK_ACCESSIBLE_PROPERTY_LABEL, "Statistics report", -1);
    gtk_box_append(GTK_BOX(controls), stats->kind);
    stats->interval = gtk_drop_down_new_from_strings((const char *[]){"1 minute", "15 minutes", "1 hour", "1 day", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(stats->interval), 2);
    gtk_accessible_update_property(GTK_ACCESSIBLE(stats->interval), GTK_ACCESSIBLE_PROPERTY_LABEL, "Time interval", -1);
    gtk_box_append(GTK_BOX(controls), stats->interval);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(controls), spacer);
    stats->search = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(stats->search), "Find a group\xe2\x80\xa6");
    gtk_widget_set_size_request(stats->search, 180, -1);
    gtk_box_append(GTK_BOX(controls), stats->search);
    gtk_box_append(GTK_BOX(content), controls);
    GtkWidget *scope_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkStringList *scopes = gtk_string_list_new(NULL);
    if (app->active) {
        char *label = app_log_name(app_log(app, app->active));
        gtk_string_list_append(scopes, label);
        g_free(label);
    }
    gtk_string_list_append(scopes, "All case events");
    stats->scope = gtk_drop_down_new(G_LIST_MODEL(scopes), NULL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(stats->scope), GTK_ACCESSIBLE_PROPERTY_LABEL, "Log scope", -1);
    gtk_box_append(GTK_BOX(scope_row), stats->scope);
    stats->limit = gtk_check_button_new_with_label("Limit to display filter");
    gboolean filtered = *stats->original_filter != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(stats->limit), filtered);
    gtk_widget_set_sensitive(stats->limit, filtered);
    gtk_widget_set_tooltip_text(stats->limit, filtered ? stats->original_filter : "No display filter applied");
    gtk_box_append(GTK_BOX(scope_row), stats->limit);
    stats->filter_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(stats->filter_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(stats->filter_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(stats->filter_label, "secondary");
    gtk_widget_set_hexpand(stats->filter_label, TRUE);
    gtk_box_append(GTK_BOX(scope_row), stats->filter_label);
    gtk_box_append(GTK_BOX(content), scope_row);
    stats->plot = gtk_drawing_area_new();
    gtk_widget_set_size_request(stats->plot, -1, 170);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(stats->plot), plot_draw, stats, NULL);
    gtk_widget_set_has_tooltip(stats->plot, TRUE);
    g_signal_connect(stats->plot, "query-tooltip", G_CALLBACK(plot_tooltip), stats);
    gtk_accessible_update_property(GTK_ACCESSIBLE(stats->plot), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Event counts by UTC time interval", -1);
    GtkGesture *press = gtk_gesture_click_new();
    g_signal_connect(press, "pressed", G_CALLBACK(plot_pressed), stats);
    gtk_widget_add_controller(stats->plot, GTK_EVENT_CONTROLLER(press));
    gtk_box_append(GTK_BOX(content), stats->plot);
    stats->store = gtk_list_store_new(S_COUNT + 2, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_DOUBLE,
                                      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING);
    stats->filtered = gtk_tree_model_filter_new(GTK_TREE_MODEL(stats->store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(stats->filtered), row_visible, stats, NULL);
    stats->sorted = gtk_tree_model_sort_new_with_model(stats->filtered);
    stats->table = gtk_tree_view_new_with_model(stats->sorted);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), stats->table);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(content), scroller);
    GtkWidget *foot = button_row(content);
    stats->message = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(stats->message), 0);
    gtk_widget_set_hexpand(stats->message, TRUE);
    gtk_box_append(GTK_BOX(foot), stats->message);
    stats->apply = gtk_button_new_with_label("Apply as filter");
    g_signal_connect(stats->apply, "clicked", G_CALLBACK(apply_clicked), stats);
    gtk_box_append(GTK_BOX(foot), stats->apply);
    GtkWidget *save = gtk_button_new_with_label("Save CSV\xe2\x80\xa6");
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), stats);
    gtk_box_append(GTK_BOX(foot), save);
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), stats);
    gtk_box_append(GTK_BOX(foot), close);
    GSimpleActionGroup *actions = g_simple_action_group_new();
    static const char *const names[] = {"apply", "copy"};
    for (int i = 0; i < 2; i++) {
        GSimpleAction *action = g_simple_action_new(names[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(copy_group), stats);
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
        g_object_unref(action);
    }
    gtk_widget_insert_action_group(stats->window, "stats", G_ACTION_GROUP(actions));
    GtkGesture *context = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context), GDK_BUTTON_SECONDARY);
    g_signal_connect(context, "pressed", G_CALLBACK(table_context), stats);
    gtk_widget_add_controller(stats->table, GTK_EVENT_CONTROLLER(context));
    g_signal_connect(stats->search, "changed", G_CALLBACK(search_changed), stats);
    g_signal_connect(stats->table, "row-activated", G_CALLBACK(row_activated), stats);
    g_signal_connect(stats->kind, "notify::selected", G_CALLBACK(control_changed), stats);
    g_signal_connect(stats->interval, "notify::selected", G_CALLBACK(control_changed), stats);
    g_signal_connect(stats->scope, "notify::selected", G_CALLBACK(control_changed), stats);
    g_signal_connect(stats->limit, "toggled", G_CALLBACK(limit_toggled), stats);
    g_signal_connect(stats->window, "close-request", G_CALLBACK(window_closed), stats);
    g_ptr_array_add(app->stats, stats->window);
    gtk_window_present(GTK_WINDOW(stats->window));
    refresh(stats);
}
