#ifndef BEC_UI_H
#define BEC_UI_H

#include <gtk/gtk.h>

#include "../io/io.h"

typedef struct App App;
typedef struct Timeline Timeline;
typedef struct Inspector Inspector;

typedef struct {
    char *query;
    char *draft;
    char *order;
    gboolean desc;
    gboolean has_selected;
    gint64 selected;
    double scroll;
} View;

typedef struct {
    gint64 id, file, total;
    char *name, *sheet, *kind, *warning;
} LogInfo;

typedef struct {
    gint64 id, rownum, number;
    char *time, *actor, *op, *result, *ip, *issue, *kind, *name, *sheet, *source, *verdict, *reason;
    GPtrArray *custom;
} Record;

struct App {
    GtkApplication *gtk;
    GtkWindow *window;
    GKeyFile *settings;
    char *settings_path;
    sqlite3 *db;
    char *path;
    GPtrArray *logs;
    GHashTable *views;
    GArray *closed_tabs;
    gint64 active;
    gboolean has_active;
    GPtrArray *history;
    gboolean busy;
    gboolean loading;
    guint query_token;
    GCancellable *query_cancel;
    GPtrArray *stats;
    // Widgets follow.
    GtkWidget *entry, *pages, *tabs, *split, *status, *scope, *warnings, *case_bar, *case_name, *case_detail,
        *case_saved, *recent_list, *welcome_case, *welcome_title, *welcome_text, *toolbar;
    Timeline *timeline;
    Inspector *inspector;
    GMenu *recent_menu, *log_menu, *filter_menu;
    GtkCssProvider *zoom_css;
    int zoom;
    gboolean switching;
    int split_position;
};

/* style.c */
extern const char *const VERDICT_COLORS[3];
void style_install(void);
GtkWidget *glyph(guint code, const char *fallback, const char *color);
GtkWidget *glyph_button(guint code, const char *fallback, const char *color, const char *tip);
GtkWidget *close_button(const char *tip);
void copy_text(GtkWidget *widget, const char *text);
void caption_colors(GtkWindow *window);
GtkWidget *pane_bar(void);
GtkWidget *dialog_window(GtkWindow *parent, const char *title, int width, int height, GtkWidget **content);
GtkWidget *button_row(GtkWidget *content);
void popup_menu(GtkWidget *parent, GMenu *menu, double x, double y);

/* window.c */
App *app_new(GtkApplication *gtk);
void app_message(App *app, const char *text);
void app_alert(App *app, const char *title, const char *detail);
gboolean app_busy(App *app);
gboolean app_load(App *app, const char *path, gboolean create);
void app_apply(App *app, const char *text);
void app_apply_in(App *app, gint64 log, const char *text);
void app_prepare(App *app, const char *text);
void app_refresh(App *app);
void app_sort(App *app, const char *key, gboolean desc);
void app_inspect(App *app);
void app_import(App *app, GPtrArray *paths);
void app_open_files_then(App *app, GPtrArray *paths);
void app_tab(App *app, gint64 key);
View *app_view(App *app);
LogInfo *app_log(App *app, gint64 id);
char *app_log_name(const LogInfo *log);
void app_saved(App *app, const char *what);
void filter_menu_items(App *app, GMenu *menu, const char *expression);
void add_text_action(GMenu *menu, const char *label, const char *action, const char *target);
void setting_set_bool(App *app, const char *key, gboolean value);
gboolean setting_bool(App *app, const char *key, gboolean fallback);
void app_set_pane(App *app, const char *key, gboolean visible);

/* timeline.c */
Timeline *timeline_new(App *app);
GtkWidget *timeline_widget(Timeline *timeline);
void timeline_reset(Timeline *timeline, sqlite3 *db, GArray *ids);
guint timeline_count(Timeline *timeline);
gint64 timeline_id(Timeline *timeline, guint position);
Record *timeline_record(Timeline *timeline, guint position);
GArray *timeline_selected(Timeline *timeline);
int timeline_current(Timeline *timeline);
void timeline_select(Timeline *timeline, guint position);
int timeline_find(Timeline *timeline, gint64 id);
gboolean timeline_add_column(Timeline *timeline, const char *label, const char *path);
void timeline_remove_column(Timeline *timeline, const char *path);
void timeline_clear_columns(Timeline *timeline);
GPtrArray *timeline_custom(Timeline *timeline);
void timeline_sort_indicator(Timeline *timeline, const char *key, gboolean desc);
void timeline_set_colors(Timeline *timeline, gboolean colors);
void timeline_fit(Timeline *timeline, gboolean forget_manual);
double timeline_scroll(Timeline *timeline);
void timeline_set_scroll(Timeline *timeline, double value);

/* inspector.c */
Inspector *inspector_new(App *app);
GtkWidget *inspector_widget(Inspector *inspector);
GtkWidget *inspector_pane(Inspector *inspector, int index);
void inspector_clear(Inspector *inspector);
void inspector_set(Inspector *inspector, sqlite3 *db, const Record *record);
void inspector_focus_find(Inspector *inspector);
void inspector_set_vertical(Inspector *inspector, gboolean vertical);

/* statistics.c */
void statistics_open(App *app, const char *key);
void statistics_close_all(App *app);

/* dialogs.c */
void dialog_help(App *app);
void dialog_saved_filters(App *app);
void dialog_save_filter(App *app);
void dialog_import_details(App *app);
void dialog_mark_time(App *app, const char *verdict);
typedef void (*TextReply)(App *app, const char *text, gpointer user);
void dialog_prompt(App *app, const char *title, const char *label, TextReply reply, gpointer user);
typedef void (*ChoiceReply)(App *app, gboolean accepted, gpointer user);
void dialog_confirm(App *app, const char *title, const char *detail, const char *accept, ChoiceReply reply, gpointer user);
GPtrArray *saved_filters(App *app);

extern const char *const EXAMPLES[][2];
extern const int EXAMPLE_COUNT;

#endif
