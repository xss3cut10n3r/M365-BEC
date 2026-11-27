#include "ui.h"

#include <string.h>

#define PAGE 160
#define PAGES 10

/* Lazy list of positions. */

#define BEC_TYPE_ROW (bec_row_get_type())
G_DECLARE_FINAL_TYPE(BecRow, bec_row, BEC, ROW, GObject)
struct _BecRow {
    GObject parent;
    guint position;
};
G_DEFINE_TYPE(BecRow, bec_row, G_TYPE_OBJECT)
static void bec_row_class_init(BecRowClass *klass) {
    (void)klass;
}
static void bec_row_init(BecRow *row) {
    (void)row;
}

#define BEC_TYPE_ROWS (bec_rows_get_type())
G_DECLARE_FINAL_TYPE(BecRows, bec_rows, BEC, ROWS, GObject)
struct _BecRows {
    GObject parent;
    guint count;
};
static GType rows_item_type(GListModel *model) {
    (void)model;
    return BEC_TYPE_ROW;
}
static guint rows_count(GListModel *model) {
    return BEC_ROWS(model)->count;
}
static gpointer rows_item(GListModel *model, guint position) {
    if (position >= BEC_ROWS(model)->count)
        return NULL;
    BecRow *row = g_object_new(BEC_TYPE_ROW, NULL);
    row->position = position;
    return row;
}
static void rows_iface(GListModelInterface *iface) {
    iface->get_item_type = rows_item_type;
    iface->get_n_items = rows_count;
    iface->get_item = rows_item;
}
G_DEFINE_TYPE_WITH_CODE(BecRows, bec_rows, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, rows_iface))
static void bec_rows_class_init(BecRowsClass *klass) {
    (void)klass;
}
static void bec_rows_init(BecRows *rows) {
    (void)rows;
}

typedef struct {
    Timeline *timeline;
    char *key;
    char *label;
    char *path;
    int minimum, weight, manual;
    GtkColumnViewColumn *column;
} Column;

typedef struct {
    gint64 page;
    guint stamp;
    Record *rows[PAGE];
} Page;

struct Timeline {
    App *app;
    GtkWidget *scroller, *view;
    BecRows *rows;
    GtkMultiSelection *selection;
    sqlite3 *db;
    GArray *ids;
    Page pages[PAGES];
    guint stamp;
    GPtrArray *columns;
    GPtrArray *custom;
    GSimpleActionGroup *actions;
    gboolean colors, fitting, sorting;
    int current;
    guint fit_source, inspect_source;
};

static const struct {
    const char *key, *label;
    int minimum, weight;
} BASE[] = {
    {"timeline_id", "ID", 62, 1},       {"time", "Time (UTC)", 154, 4}, {"source", "Source", 108, 7},
    {"name", "Source file", 111, 10},   {"actor", "Actor / account", 124, 10}, {"ip", "IP address", 104, 6},
    {"op", "Operation", 110, 9},        {"result", "Result", 72, 3},
};

static void record_free(Record *record) {
    if (!record)
        return;
    g_free(record->time);
    g_free(record->actor);
    g_free(record->op);
    g_free(record->result);
    g_free(record->ip);
    g_free(record->issue);
    g_free(record->kind);
    g_free(record->name);
    g_free(record->sheet);
    g_free(record->source);
    g_free(record->verdict);
    g_free(record->reason);
    g_ptr_array_free(record->custom, TRUE);
    g_free(record);
}

static void clear_pages(Timeline *timeline) {
    for (int i = 0; i < PAGES; i++) {
        for (int k = 0; k < PAGE; k++)
            g_clear_pointer(&timeline->pages[i].rows[k], record_free);
        timeline->pages[i].page = -1;
        timeline->pages[i].stamp = 0;
    }
}

guint timeline_count(Timeline *timeline) {
    return timeline->ids ? timeline->ids->len : 0;
}

gint64 timeline_id(Timeline *timeline, guint position) {
    return g_array_index(timeline->ids, gint64, position);
}

// Cache visible timeline pages.
Record *timeline_record(Timeline *timeline, guint position) {
    if (!timeline->db || position >= timeline_count(timeline))
        return NULL;
    gint64 number = position / PAGE;
    Page *page = NULL, *oldest = &timeline->pages[0];
    for (int i = 0; i < PAGES; i++) {
        if (timeline->pages[i].page == number)
            page = &timeline->pages[i];
        if (timeline->pages[i].stamp < oldest->stamp)
            oldest = &timeline->pages[i];
    }
    if (!page) {
        page = oldest;
        for (int k = 0; k < PAGE; k++)
            g_clear_pointer(&page->rows[k], record_free);
        page->page = number;
        guint start = (guint)number * PAGE, end = MIN(start + PAGE, timeline_count(timeline));
        GPtrArray *args = args_new();
        char *select = select_sql(timeline->custom, args);
        GString *sql = g_string_new(select);
        g_string_append(sql, " WHERE e.id IN (");
        GHashTable *slots = g_hash_table_new(g_int64_hash, g_int64_equal);
        for (guint i = start; i < end; i++) {
            g_string_append(sql, i > start ? ",?" : "?");
            args_int(args, timeline_id(timeline, i));
            g_hash_table_insert(slots, &g_array_index(timeline->ids, gint64, i), GUINT_TO_POINTER(i - start + 1));
        }
        g_string_append_c(sql, ')');
        sqlite3_stmt *statement = sql_prepare(timeline->db, sql->str, NULL);
        if (statement) {
            args_bind(statement, args, 1);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                gint64 id = sqlite3_column_int64(statement, 0);
                guint slot = GPOINTER_TO_UINT(g_hash_table_lookup(slots, &id));
                if (!slot)
                    continue;
                Record *record = g_new0(Record, 1);
                record->id = id;
                record->rownum = sqlite3_column_int64(statement, 1);
                record->time = g_strdup(column_text(statement, 2));
                record->actor = g_strdup(column_text(statement, 3));
                record->op = g_strdup(column_text(statement, 4));
                record->result = g_strdup(column_text(statement, 5));
                record->ip = g_strdup(column_text(statement, 6));
                record->issue = g_strdup(column_text(statement, 7));
                record->kind = g_strdup(column_text(statement, 8));
                record->name = g_strdup(column_text(statement, 9));
                record->sheet = g_strdup(column_text(statement, 10));
                record->source = g_strdup(column_text(statement, 11));
                record->number = sqlite3_column_int64(statement, 12);
                record->verdict = g_strdup(column_text(statement, 13));
                record->reason = g_strdup(column_text(statement, 14));
                record->custom = g_ptr_array_new_with_free_func(g_free);
                for (guint i = 0; i < timeline->custom->len; i++)
                    g_ptr_array_add(record->custom, g_strdup(column_text(statement, 15 + (int)i)));
                page->rows[slot - 1] = record;
            }
            sqlite3_finalize(statement);
        }
        g_hash_table_destroy(slots);
        g_string_free(sql, TRUE);
        g_free(select);
        g_ptr_array_free(args, TRUE);
    }
    page->stamp = ++timeline->stamp;
    return page->rows[position % PAGE];
}

static char *display_time(const char *value) {
    if (!*value)
        return g_strdup("");
    GString *out = g_string_new_len(value, MIN(strlen(value), 19));
    g_string_replace(out, "T", " ", 0);
    if (strchr(value, '.') && strlen(value) > 20) {
        char *fraction = g_strndup(value + 19, strlen(value) - 20);
        size_t length = strlen(fraction);
        while (length && fraction[length - 1] == '0')
            fraction[--length] = 0;
        if (length && fraction[length - 1] == '.')
            fraction[--length] = 0;
        g_string_append(out, fraction);
        g_free(fraction);
    }
    return g_string_free(out, FALSE);
}

static char *cell_value(Timeline *timeline, Record *record, Column *column) {
    const char *key = column->key;
    if (column->path) {
        for (guint i = 0; i < timeline->custom->len; i++)
            if (!strcmp(((CustomColumn *)timeline->custom->pdata[i])->path, column->path))
                return g_strdup(i < record->custom->len ? record->custom->pdata[i] : "");
        return g_strdup("");
    }
    if (!strcmp(key, "timeline_id"))
        return g_strdup_printf("%" G_GINT64_FORMAT, record->number);
    const char *text = !strcmp(key, "time") ? record->time
                       : !strcmp(key, "source") ? record->source
                       : !strcmp(key, "name") ? record->name
                       : !strcmp(key, "actor") ? record->actor
                       : !strcmp(key, "ip") ? record->ip
                       : !strcmp(key, "op") ? record->op : record->result;
    return g_strdup(text);
}

static void show_context(Timeline *timeline, GtkWidget *cell, Column *column, guint position, double x, double y);

static void cell_pressed(GtkGestureClick *gesture, int presses, double x, double y, gpointer user) {
    (void)presses;
    Column *column = user;
    Timeline *timeline = column->timeline;
    GtkWidget *label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkListItem *item = g_object_get_data(G_OBJECT(label), "item");
    guint position = item ? gtk_list_item_get_position(item) : GTK_INVALID_LIST_POSITION;
    if (position == GTK_INVALID_LIST_POSITION)
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(timeline->selection), position))
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(timeline->selection), position, TRUE);
    timeline->current = (int)position;
    show_context(timeline, label, column, position, x, y);
}

static void cell_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
    (void)factory;
    Column *column = user;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), !strcmp(column->key, "timeline_id") ? 1.0f : 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    if (!strcmp(column->key, "timeline_id") || !strcmp(column->key, "time") || !strcmp(column->key, "ip"))
        gtk_widget_add_css_class(label, "mono");
    if (!strcmp(column->key, "timeline_id"))
        gtk_widget_add_css_class(label, "number");
    g_object_set_data(G_OBJECT(label), "item", item);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(cell_pressed), column);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
    gtk_list_item_set_child(item, label);
}

static void cell_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
    (void)factory;
    Column *column = user;
    Timeline *timeline = column->timeline;
    GtkWidget *label = gtk_list_item_get_child(item);
    BecRow *row = gtk_list_item_get_item(item);
    Record *record = row ? timeline_record(timeline, row->position) : NULL;
    char *value = record ? cell_value(timeline, record, column) : g_strdup("");
    char *shown = !strcmp(column->key, "time") ? display_time(value) : g_strdup(value);
    // Single lines keep rows compact.
    for (char *p = shown; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    gtk_label_set_text(GTK_LABEL(label), shown);
    gtk_widget_set_tooltip_text(label, *value ? value : NULL);
    GtkWidget *cell = gtk_widget_get_parent(label);
    GtkWidget *line = cell ? gtk_widget_get_parent(cell) : NULL;
    if (line) {
        static const char *const classes[] = {"good", "bad", "unsure"};
        for (int i = 0; i < 3; i++) {
            gboolean on = timeline->colors && record && !g_ascii_strcasecmp(record->verdict, classes[i]);
            if (on)
                gtk_widget_add_css_class(line, classes[i]);
            else
                gtk_widget_remove_css_class(line, classes[i]);
        }
    }
    g_free(shown);
    g_free(value);
}

static int never_sorts(gconstpointer a, gconstpointer b, gpointer user) {
    (void)a;
    (void)b;
    (void)user;
    return 0;
}

static void column_free(gpointer data) {
    Column *column = data;
    g_free(column->key);
    g_free(column->label);
    g_free(column->path);
    g_free(column);
}

static gboolean fit_idle(gpointer user) {
    Timeline *timeline = user;
    timeline->fit_source = 0;
    timeline_fit(timeline, FALSE);
    return G_SOURCE_REMOVE;
}

static void schedule_fit(Timeline *timeline) {
    if (!timeline->fitting && !timeline->fit_source)
        timeline->fit_source = g_idle_add(fit_idle, timeline);
}

static void width_changed(GtkColumnViewColumn *view_column, GParamSpec *spec, gpointer user) {
    (void)spec;
    Column *column = user;
    if (column->timeline->fitting)
        return;
    int width = gtk_column_view_column_get_fixed_width(view_column);
    if (width > 0)
        column->manual = width;
    schedule_fit(column->timeline);
}

static void visibility_changed(GSimpleAction *action, GVariant *state, gpointer user) {
    Column *column = user;
    g_simple_action_set_state(action, state);
    gtk_column_view_column_set_visible(column->column, g_variant_get_boolean(state));
    schedule_fit(column->timeline);
}

static void rebuild_header_menus(Timeline *timeline) {
    for (guint i = 0; i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        GMenu *menu = g_menu_new(), *toggles = g_menu_new(), *tools = g_menu_new();
        for (guint k = 0; k < timeline->columns->len; k++) {
            Column *other = timeline->columns->pdata[k];
            char *action = g_strdup_printf("timeline.show-%p", (void *)other);
            g_menu_append(toggles, other->label, action);
            g_free(action);
        }
        g_menu_append(tools, "Fit columns to window", "timeline.fit");
        if (column->path)
            add_text_action(tools, "Remove custom column", "timeline.remove", column->path);
        g_menu_append_section(menu, NULL, G_MENU_MODEL(toggles));
        g_menu_append_section(menu, NULL, G_MENU_MODEL(tools));
        gtk_column_view_column_set_header_menu(column->column, G_MENU_MODEL(menu));
        g_object_unref(menu);
        g_object_unref(toggles);
        g_object_unref(tools);
    }
}

static Column *add_view_column(Timeline *timeline, const char *key, const char *label, const char *path, int minimum,
                               int weight, int position) {
    Column *column = g_new0(Column, 1);
    column->timeline = timeline;
    column->key = g_strdup(key);
    column->label = g_strdup(label);
    column->path = g_strdup(path);
    column->minimum = minimum;
    column->weight = weight;
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(cell_setup), column);
    g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), column);
    column->column = gtk_column_view_column_new(label, factory);
    gtk_column_view_column_set_resizable(column->column, TRUE);
    gtk_column_view_column_set_fixed_width(column->column, minimum);
    GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(never_sorts, NULL, NULL));
    gtk_column_view_column_set_sorter(column->column, sorter);
    g_object_unref(sorter);
    g_signal_connect(column->column, "notify::fixed-width", G_CALLBACK(width_changed), column);
    char *name = g_strdup_printf("show-%p", (void *)column);
    GSimpleAction *action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(TRUE));
    g_signal_connect(action, "change-state", G_CALLBACK(visibility_changed), column);
    g_action_map_add_action(G_ACTION_MAP(timeline->actions), G_ACTION(action));
    g_object_unref(action);
    g_free(name);
    if (position < 0) {
        gtk_column_view_append_column(GTK_COLUMN_VIEW(timeline->view), column->column);
        g_ptr_array_add(timeline->columns, column);
    } else {
        gtk_column_view_insert_column(GTK_COLUMN_VIEW(timeline->view), (guint)position, column->column);
        g_ptr_array_insert(timeline->columns, position, column);
    }
    return column;
}

// Size columns within available space.
void timeline_fit(Timeline *timeline, gboolean forget_manual) {
    int available = gtk_widget_get_width(timeline->scroller) - 2;
    GtkWidget *bar = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(timeline->scroller));
    if (bar && gtk_widget_get_visible(bar) && !gtk_scrolled_window_get_overlay_scrolling(GTK_SCROLLED_WINDOW(timeline->scroller)))
        available -= gtk_widget_get_width(bar);
    if (available <= 0)
        return;
    int used = 0, weights = 0;
    gboolean all_manual = TRUE;
    for (guint i = 0; i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        if (forget_manual)
            column->manual = 0;
        if (!gtk_column_view_column_get_visible(column->column))
            continue;
        used += MAX(column->minimum, column->manual);
        all_manual = all_manual && column->manual > 0;
    }
    for (guint i = 0; i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        if (gtk_column_view_column_get_visible(column->column) && (all_manual || !column->manual))
            weights += column->weight;
    }
    int extra = MAX(0, available - used);
    timeline->fitting = TRUE;
    for (guint i = 0; i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        if (!gtk_column_view_column_get_visible(column->column))
            continue;
        int width = MAX(column->minimum, column->manual);
        if ((all_manual || !column->manual) && weights > 0) {
            int share = extra * column->weight / weights;
            width += share;
            extra -= share;
            weights -= column->weight;
        }
        if (gtk_column_view_column_get_fixed_width(column->column) != width)
            gtk_column_view_column_set_fixed_width(column->column, width);
    }
    timeline->fitting = FALSE;
}

static void page_size_changed(GObject *adjustment, GParamSpec *spec, gpointer user) {
    (void)adjustment;
    (void)spec;
    schedule_fit(user);
}

static void sort_changed(GtkSorter *sorter, GtkSorterChange change, gpointer user) {
    (void)change;
    Timeline *timeline = user;
    if (timeline->sorting)
        return;
    GtkColumnViewSorter *view_sorter = GTK_COLUMN_VIEW_SORTER(sorter);
    GtkColumnViewColumn *primary = gtk_column_view_sorter_get_primary_sort_column(view_sorter);
    for (guint i = 0; primary && i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        if (column->column != primary)
            continue;
        char *key = column->path ? g_strconcat("ptr:", column->path, NULL) : g_strdup(column->key);
        app_sort(timeline->app, key, gtk_column_view_sorter_get_primary_sort_order(view_sorter) == GTK_SORT_DESCENDING);
        g_free(key);
        return;
    }
}

void timeline_sort_indicator(Timeline *timeline, const char *key, gboolean desc) {
    timeline->sorting = TRUE;
    for (guint i = 0; i < timeline->columns->len; i++) {
        Column *column = timeline->columns->pdata[i];
        gboolean match = column->path ? g_str_has_prefix(key, "ptr:") && !strcmp(key + 4, column->path)
                                      : !strcmp(key, column->key);
        if (match)
            gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(timeline->view), column->column,
                                           desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    }
    timeline->sorting = FALSE;
}

static gboolean inspect_idle(gpointer user) {
    Timeline *timeline = user;
    timeline->inspect_source = 0;
    app_inspect(timeline->app);
    return G_SOURCE_REMOVE;
}

static void selection_changed(GtkSelectionModel *model, guint position, guint count, gpointer user) {
    Timeline *timeline = user;
    int found = -1;
    for (guint i = position; i < position + count && found < 0; i++)
        if (gtk_selection_model_is_selected(model, i))
            found = (int)i;
    if (found < 0) {
        GtkBitset *selected = gtk_selection_model_get_selection(model);
        if (!gtk_bitset_is_empty(selected))
            found = (int)gtk_bitset_get_minimum(selected);
        gtk_bitset_unref(selected);
    }
    timeline->current = found;
    if (!timeline->inspect_source)
        timeline->inspect_source = g_idle_add(inspect_idle, timeline);
}

static void fit_action(GSimpleAction *action, GVariant *parameter, gpointer user) {
    (void)action;
    (void)parameter;
    timeline_fit(user, TRUE);
}

static void remove_action(GSimpleAction *action, GVariant *parameter, gpointer user) {
    (void)action;
    Timeline *timeline = user;
    char *path = g_variant_dup_string(parameter, NULL);
    timeline_remove_column(timeline, path);
    app_refresh(timeline->app);
    g_free(path);
}

// Redraws rows, keeping the selection.
static void redraw_rows(Timeline *timeline) {
    GtkSelectionModel *model = GTK_SELECTION_MODEL(timeline->selection);
    GtkBitset *selected = gtk_selection_model_get_selection(model);
    GtkBitset *kept = gtk_bitset_copy(selected);
    gtk_bitset_unref(selected);
    int current = timeline->current;
    g_list_model_items_changed(G_LIST_MODEL(timeline->rows), 0, timeline->rows->count, timeline->rows->count);
    if (!gtk_bitset_is_empty(kept))
        gtk_selection_model_set_selection(model, kept, kept);
    gtk_bitset_unref(kept);
    timeline->current = current;
}

Timeline *timeline_new(App *app) {
    Timeline *timeline = g_new0(Timeline, 1);
    timeline->app = app;
    timeline->colors = TRUE;
    timeline->current = -1;
    timeline->columns = g_ptr_array_new_with_free_func(column_free);
    timeline->custom = g_ptr_array_new();
    timeline->rows = g_object_new(BEC_TYPE_ROWS, NULL);
    timeline->selection = gtk_multi_selection_new(G_LIST_MODEL(timeline->rows));
    clear_pages(timeline);
    timeline->view = gtk_column_view_new(GTK_SELECTION_MODEL(timeline->selection));
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(timeline->view), TRUE);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(timeline->view), FALSE);
    gtk_widget_set_name(timeline->view, "timeline");
    timeline->actions = g_simple_action_group_new();
    GSimpleAction *fit = g_simple_action_new("fit", NULL);
    g_signal_connect(fit, "activate", G_CALLBACK(fit_action), timeline);
    g_action_map_add_action(G_ACTION_MAP(timeline->actions), G_ACTION(fit));
    GSimpleAction *remove = g_simple_action_new("remove", G_VARIANT_TYPE_STRING);
    g_signal_connect(remove, "activate", G_CALLBACK(remove_action), timeline);
    g_action_map_add_action(G_ACTION_MAP(timeline->actions), G_ACTION(remove));
    gtk_widget_insert_action_group(timeline->view, "timeline", G_ACTION_GROUP(timeline->actions));
    for (guint i = 0; i < G_N_ELEMENTS(BASE); i++)
        add_view_column(timeline, BASE[i].key, BASE[i].label, NULL, BASE[i].minimum, BASE[i].weight, -1);
    rebuild_header_menus(timeline);
    timeline->scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(timeline->scroller), timeline->view);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(timeline->scroller), FALSE);
    gtk_widget_set_vexpand(timeline->scroller, TRUE);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(timeline->scroller)), "notify::page-size",
                     G_CALLBACK(page_size_changed), timeline);
    g_signal_connect(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(timeline->view)), "changed", G_CALLBACK(sort_changed),
                     timeline);
    g_signal_connect(timeline->selection, "selection-changed", G_CALLBACK(selection_changed), timeline);
    return timeline;
}

GtkWidget *timeline_widget(Timeline *timeline) {
    return timeline->scroller;
}

void timeline_reset(Timeline *timeline, sqlite3 *db, GArray *ids) {
    guint before = timeline->rows->count;
    clear_pages(timeline);
    if (timeline->ids)
        g_array_free(timeline->ids, TRUE);
    timeline->db = db;
    timeline->ids = ids;
    timeline->current = -1;
    timeline->rows->count = ids ? ids->len : 0;
    g_list_model_items_changed(G_LIST_MODEL(timeline->rows), 0, before, timeline->rows->count);
    schedule_fit(timeline);
}

GArray *timeline_selected(Timeline *timeline) {
    GArray *positions = g_array_new(FALSE, FALSE, sizeof(guint));
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(timeline->selection));
    GtkBitsetIter iter;
    guint value;
    if (gtk_bitset_iter_init_first(&iter, selected, &value)) {
        do
            g_array_append_val(positions, value);
        while (gtk_bitset_iter_next(&iter, &value));
    }
    gtk_bitset_unref(selected);
    return positions;
}

int timeline_current(Timeline *timeline) {
    return timeline->current >= 0 && (guint)timeline->current < timeline_count(timeline) ? timeline->current : -1;
}

void timeline_select(Timeline *timeline, guint position) {
    if (position >= timeline_count(timeline))
        return;
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(timeline->view), position, NULL,
                              GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
    timeline->current = (int)position;
}

int timeline_find(Timeline *timeline, gint64 id) {
    for (guint i = 0; i < timeline_count(timeline); i++)
        if (timeline_id(timeline, i) == id)
            return (int)i;
    return -1;
}

GPtrArray *timeline_custom(Timeline *timeline) {
    return timeline->custom;
}

gboolean timeline_add_column(Timeline *timeline, const char *label, const char *path) {
    for (guint i = 0; i < timeline->custom->len; i++)
        if (!strcmp(((CustomColumn *)timeline->custom->pdata[i])->path, path))
            return FALSE;
    CustomColumn *custom = g_new0(CustomColumn, 1);
    custom->label = g_strdup(label);
    custom->path = g_strdup(path);
    g_ptr_array_add(timeline->custom, custom);
    clear_pages(timeline);
    // Place fields after Operation.
    GListModel *order = gtk_column_view_get_columns(GTK_COLUMN_VIEW(timeline->view));
    int position = -1;
    for (guint i = 0; i < g_list_model_get_n_items(order); i++) {
        GtkColumnViewColumn *item = g_list_model_get_item(order, i);
        for (guint k = 0; k < timeline->columns->len; k++) {
            Column *column = timeline->columns->pdata[k];
            if (column->column == item && !column->path && !strcmp(column->key, "op"))
                position = (int)i + 1;
        }
        g_object_unref(item);
    }
    Column *column = add_view_column(timeline, "custom", label, path, 108, 7, position);
    gtk_widget_set_tooltip_text(timeline->view, NULL);
    (void)column;
    rebuild_header_menus(timeline);
    redraw_rows(timeline);
    schedule_fit(timeline);
    return TRUE;
}

void timeline_remove_column(Timeline *timeline, const char *path) {
    for (guint i = 0; i < timeline->custom->len; i++) {
        CustomColumn *custom = timeline->custom->pdata[i];
        if (strcmp(custom->path, path))
            continue;
        for (guint k = 0; k < timeline->columns->len; k++) {
            Column *column = timeline->columns->pdata[k];
            if (column->path && !strcmp(column->path, path)) {
                char *name = g_strdup_printf("show-%p", (void *)column);
                g_action_map_remove_action(G_ACTION_MAP(timeline->actions), name);
                g_free(name);
                gtk_column_view_remove_column(GTK_COLUMN_VIEW(timeline->view), column->column);
                g_ptr_array_remove_index(timeline->columns, k);
                break;
            }
        }
        g_free(custom->label);
        g_free(custom->path);
        g_free(custom);
        g_ptr_array_remove_index(timeline->custom, i);
        break;
    }
    clear_pages(timeline);
    rebuild_header_menus(timeline);
    schedule_fit(timeline);
}

void timeline_clear_columns(Timeline *timeline) {
    while (timeline->custom->len) {
        char *path = g_strdup(((CustomColumn *)timeline->custom->pdata[0])->path);
        timeline_remove_column(timeline, path);
        g_free(path);
    }
}

void timeline_set_colors(Timeline *timeline, gboolean colors) {
    timeline->colors = colors;
    redraw_rows(timeline);
}

double timeline_scroll(Timeline *timeline) {
    return gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(timeline->scroller)));
}

typedef struct {
    Timeline *timeline;
    double value;
    int frames;
} Scroll;

// Wait until rows are laid out.
static gboolean scroll_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget;
    (void)clock;
    Scroll *scroll = data;
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll->timeline->scroller));
    double limit = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    if (limit + 1 < scroll->value && ++scroll->frames < 30)
        return G_SOURCE_CONTINUE;
    gtk_adjustment_set_value(adjustment, MIN(scroll->value, MAX(limit, 0)));
    return G_SOURCE_REMOVE;
}

void timeline_set_scroll(Timeline *timeline, double value) {
    Scroll *scroll = g_new0(Scroll, 1);
    scroll->timeline = timeline;
    scroll->value = value;
    gtk_widget_add_tick_callback(timeline->scroller, scroll_tick, scroll, g_free);
}

/* Right-click actions for cells. */

static void mark_menu(GMenu *menu, const char *title, const char *action) {
    GMenu *sub = g_menu_new();
    for (int i = 0; i < 4; i++)
        add_text_action(sub, i == 3 ? "Clear verdict" : VERDICTS[i], action, VERDICTS[i]);
    g_menu_append_submenu(menu, title, G_MENU_MODEL(sub));
    g_object_unref(sub);
}

static void follow_items(GMenu *menu, const char *field, const char *label, GPtrArray *values) {
    for (guint i = 0; i < values->len; i++) {
        const char *value = values->pdata[i];
        if (!*value)
            continue;
        char *quoted = j_quoted(value);
        char *expression = g_strdup_printf("%s = %s", field, quoted);
        char *brief = g_utf8_substring(value, 0, MIN(55, g_utf8_strlen(value, -1)));
        char *title = g_strdup_printf("%s \xc2\xb7 %s", label, brief);
        add_text_action(menu, title, "win.filter-case", expression);
        g_free(title);
        g_free(brief);
        g_free(expression);
        g_free(quoted);
    }
}

static void show_context(Timeline *timeline, GtkWidget *cell, Column *column, guint position, double x, double y) {
    Record *record = timeline_record(timeline, position);
    if (!record)
        return;
    sqlite3_stmt *origin = sql_prepare(timeline->db,
                                       "SELECT l.headers,e.raw,e.request FROM events e JOIN logs l ON l.id=e.log WHERE e.id=?",
                                       NULL);
    if (!origin)
        return;
    sqlite3_bind_int64(origin, 1, record->id);
    if (sqlite3_step(origin) != SQLITE_ROW) {
        sqlite3_finalize(origin);
        return;
    }
    const char *headers = column_text(origin, 0), *raw = column_text(origin, 1);
    char *request = g_strdup(column_text(origin, 2));
    char *value = cell_value(timeline, record, column);
    GMenu *menu = g_menu_new(), *marks = g_menu_new(), *filters = g_menu_new(), *copies = g_menu_new();
    mark_menu(marks, "Mark selected events", "win.mark-events");
    mark_menu(marks, "Mark selected IPs", "win.mark-ips");
    mark_menu(marks, "Mark UTC time range", "win.mark-range");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(marks));
    GString *expression = g_string_new(NULL);
    Columns *columns = columns_new(headers);
    if (column->path) {
        char *pointer = j_quoted(column->path);
        GPtrArray *found = pointer_values(columns, raw, column->path, FALSE);
        for (guint i = 0; i < found->len; i++) {
            gboolean repeat = FALSE;
            for (guint k = 0; k < i && !repeat; k++)
                repeat = !strcmp(found->pdata[k], found->pdata[i]);
            if (repeat)
                continue;
            char *quoted = j_quoted(found->pdata[i]);
            g_string_append_printf(expression, "%sfield %s = %s", expression->len ? " OR " : "", pointer, quoted);
            g_free(quoted);
        }
        g_ptr_array_free(found, TRUE);
        g_free(pointer);
    } else {
        const char *field = !strcmp(column->key, "timeline_id") ? "id"
                            : !strcmp(column->key, "name") ? "file"
                            : !strcmp(column->key, "op") ? "operation" : column->key;
        char **items = !strcmp(column->key, "ip") ? g_strsplit(value, ", ", -1) : g_strsplit(value, "\x1f\x1f", 1);
        for (char **item = items; *item; item++) {
            if (!**item)
                continue;
            char *quoted = j_quoted(*item);
            g_string_append_printf(expression, "%s%s = %s", expression->len ? " OR " : "", field, quoted);
            g_free(quoted);
        }
        g_strfreev(items);
    }
    if (expression->len)
        filter_menu_items(timeline->app, filters, expression->str);
    GMenu *follow = g_menu_new();
    EventFields *fields = event_fields_new(headers, raw);
    static const char *const slots[][2] = {{"session", "Session"}, {"device", "Device"}};
    GPtrArray *single = g_ptr_array_new();
    g_ptr_array_add(single, record->actor);
    follow_items(follow, "mailbox", "Account", single);
    for (guint i = 0; i < G_N_ELEMENTS(slots); i++) {
        GPtrArray *values = event_values(fields, slots[i][0]);
        follow_items(follow, slots[i][0], slots[i][1], values);
        g_ptr_array_free(values, TRUE);
    }
    g_ptr_array_set_size(single, 0);
    g_ptr_array_add(single, request);
    follow_items(follow, "request", "Authentication request", single);
    static const char *const later[][2] = {{"messageid", "Message"}, {"correlation", "Correlation"}};
    for (guint i = 0; i < G_N_ELEMENTS(later); i++) {
        GPtrArray *values = event_values(fields, later[i][0]);
        follow_items(follow, later[i][0], later[i][1], values);
        g_ptr_array_free(values, TRUE);
    }
    g_ptr_array_free(single, TRUE);
    if (g_menu_model_get_n_items(G_MENU_MODEL(follow)))
        g_menu_append_submenu(filters, "Follow", G_MENU_MODEL(follow));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(filters));
    GMenu *copy = g_menu_new();
    add_text_action(copy, "Cell value", "win.copy-text", value);
    g_menu_append(copy, "Copy event with source reference", "win.copy");
    char *reference = source_location(record->name, record->sheet, record->rownum);
    add_text_action(copy, "Source reference", "win.copy-text", reference);
    g_menu_append_submenu(copies, "Copy", G_MENU_MODEL(copy));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(copies));
    popup_menu(cell, menu, x, y);
    g_free(reference);
    g_object_unref(copy);
    g_object_unref(follow);
    g_object_unref(marks);
    g_object_unref(filters);
    g_object_unref(copies);
    g_object_unref(menu);
    event_fields_free(fields);
    columns_free(columns);
    g_string_free(expression, TRUE);
    g_free(value);
    g_free(request);
    sqlite3_finalize(origin);
}
