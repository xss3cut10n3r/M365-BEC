#include "ui.h"

#ifdef G_OS_WIN32
#include <dwmapi.h>
#include <gdk/win32/gdkwin32.h>
#endif

const char *const VERDICT_COLORS[3] = {"#39FF14", "#FF2020", "#FFFF00"};

static const char CSS[] =
    "window, popover, tooltip { font-family: 'Segoe UI', 'Noto Sans', 'DejaVu Sans', sans-serif; font-size: 9pt; }"
    "window, .background { background: #2c2c2c; color: #ededed; }"
    "popovermenubar { background: #202122; border-bottom: 1px solid #525455; padding: 1px; min-height: 0; }"
    "popovermenubar > item { padding: 3px 7px; min-height: 0; border-radius: 0; }"
    "popovermenubar > item:hover, popovermenubar > item:selected, popover.menu modelbutton:hover,"
    "popover.menu modelbutton:selected { background: #485362; }"
    "popover.menu > contents, popover.background > contents { background: #303233; border: 1px solid #575b5d;"
    " padding: 3px; border-radius: 2px; color: #ededed; }"
    "popover.menu modelbutton { padding: 3px 9px; min-height: 0; border-radius: 0; }"
    "popover.menu separator { margin: 3px 0; background: #575b5d; }"
    ".toolbar { background: #202122; border-bottom: 1px solid #55585b; padding: 2px; }"
    ".toolbar separator { min-width: 1px; background: #535657; margin: 4px; }"
    "button { background: #343638; color: #e4e7e8; border: 1px solid #55585a; border-radius: 3px; padding: 3px 9px;"
    " min-height: 0; min-width: 0; box-shadow: none; text-shadow: none; background-image: none; }"
    "button:hover { background: #404548; border-color: #646a6c; }"
    "button:active, button:checked { background: #50545a; }"
    "button:disabled { color: #86898b; }"
    "button:disabled label, button:disabled .glyph { opacity: 0.35; }"
    "button.tool, menubutton.tool > button { background: transparent; border: 1px solid transparent; border-radius: 2px;"
    " padding: 3px; }"
    "button.tool:hover, menubutton.tool > button:hover { background: #404548; border-color: #646a6c; }"
    "button.tool:active, menubutton.tool > button:checked { background: #50545a; }"
    "button.link { background: transparent; border: 0; color: #aacdeb; padding: 4px; }"
    "button.link:hover { background: #343638; }"
    "button.link > label { text-decoration: none; }"
    ".statusbar separator { background: #515457; min-width: 1px; margin: 2px 0; }"
    ".filterBar { background: #222324; border-bottom: 1px solid #55585a; padding: 3px; }"
    ".paneBar { background: #333536; border-bottom: 1px solid #505355; padding: 2px 4px; }"
    ".caseName { font-weight: 600; color: #ffffff; background: #3d5566; border-radius: 3px; padding: 1px 9px; }"
    ".caseName.none { background: transparent; color: #b8bfc1; font-weight: 400; padding: 1px 2px; }"
    ".secondary { color: #b8bfc1; }"
    ".saved { color: #9fd9b4; }"
    "entry, textview, textview text, dropdown > button { background: #252728; color: #ededed; }"
    "entry { border: 1px solid #5c6062; border-radius: 2px; padding: 2px 6px; min-height: 0; box-shadow: none;"
    " outline: none; }"
    "entry:focus-within { border-color: #8bacbf; }"
    "entry.valid { background: #273a30; border-color: #719683; }"
    "entry.error { background: #442d30; border-color: #bc7c79; }"
    "entry.mono, .mono { font-family: Consolas, 'DejaVu Sans Mono', monospace; }"
    "entry selection, textview text selection { background: #6b537b; color: #ffffff; }"
    "dropdown > button { padding: 2px 5px; border: 1px solid #626568; border-radius: 2px; }"
    "notebook, notebook > header { background: #2c2c2c; border: 0; box-shadow: none; padding: 0; min-height: 0; }"
    "notebook > header > tabs { margin: 0; padding: 0; }"
    "notebook > header tab { background: #252728; border: 0; border-top: 2px solid transparent; padding: 4px 4px 4px 8px;"
    " margin: 0 1px 0 0; min-height: 0; border-radius: 0; color: #ededed; box-shadow: none; outline: none; }"
    "notebook > header tab:hover { background: #303334; box-shadow: none; }"
    "notebook > header tab:checked { background: #383b3d; border-top: 2px solid #a4c9e9; box-shadow: none; }"
    "notebook > header tab:focus, notebook > header tab:focus-visible { box-shadow: none; outline: none; }"
    "notebook > stack { min-height: 0; background: transparent; }"
    "button.closeButton { padding: 0; border: 0; background: transparent; min-width: 22px; min-height: 22px;"
    " border-radius: 2px; }"
    "button.closeButton:hover, button.closeButton:focus { background: #575b5e; }"
    "button.closeButton:active { background: #694449; }"
    "columnview, treeview, treeview.view, listview { background: #262829; color: #ededed; }"
    "columnview > header > button, treeview.view header button { background: #353739; border: 0; border-radius: 0;"
    " border-right: 1px solid #55585a; border-bottom: 1px solid #545758; padding: 3px 6px; color: #e3e6e7;"
    " font-weight: 700; }"
    "columnview > listview > row { padding: 0; min-height: 0; }"
    "columnview > listview > row > cell { padding: 1px 4px; min-height: 0; }"
    "columnview > listview > row:nth-child(even), treeview.view:nth-child(even) { background: #2c2e2f; }"
    "columnview > listview > row:selected { background: #474b4e; color: #ffffff; }"
    "columnview > listview > row.good { background: #39FF14; color: #101010; }"
    "columnview > listview > row.bad { background: #FF2020; color: #101010; }"
    "columnview > listview > row.unsure { background: #FFFF00; color: #101010; }"
    "columnview > listview > row.good:selected, columnview > listview > row.bad:selected,"
    "columnview > listview > row.unsure:selected { box-shadow: inset 0 2px #171717, inset 0 -2px #171717; }"
    "columnview .number { color: #afbbbf; }"
    "columnview row.good .number, columnview row.bad .number, columnview row.unsure .number { color: #101010; }"
    "treeview.view { padding: 1px 4px; }"
    "treeview.view:selected { background: #754086; color: #ffffff; }"
    "paned > separator { background: #535657; min-width: 3px; min-height: 3px; }"
    "paned > separator:hover { background: #829eac; }"
    ".statusbar { background: #202122; border-top: 1px solid #515457; padding: 2px 6px; }"
    ".warning { color: #f0c68d; }"
    "tooltip, tooltip.background { background: #383b3d; color: #eeeeee; border: 1px solid #686d70; padding: 3px; }"
    "button.link > label { text-decoration: none; }"
    ".statusbar separator { background: #515457; min-width: 1px; margin: 2px 0; }"
    ".banner { background: #b94f00; color: #ffffff; padding: 7px 11px; border-radius: 3px; }"
    ".heading { font-size: 13pt; font-weight: 600; color: #bcc3c5; padding: 14px 0 6px 0; }"
    ".caseFile { color: #bcc3c5; font-weight: 600; font-size: 15pt; padding-bottom: 2px; }"
    ".recent { background: #262829; }"
    ".recent row { padding: 3px 6px; }"
    ".recent row:nth-child(even) { background: #2c2e2f; }"
    ".recent row:selected, .recent row:nth-child(even):selected { background: #474b4e; color: #ffffff; }"
    ".plot { background: #242729; }"
    "scrollbar { background: #2c2c2c; }"
    "scrollbar slider { background: #5a5d5f; min-width: 8px; min-height: 8px; border-radius: 4px; }"
    "checkbutton check { background: #252728; border: 1px solid #626568; }";

void style_install(void) {
    GtkSettings *settings = gtk_settings_get_default();
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings), "gtk-interface-color-scheme"))
        g_object_set(settings, "gtk-interface-color-scheme", 2, NULL);
    else
        g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

typedef struct {
    guint code;
    GdkRGBA color;
} Glyph;

static void line(cairo_t *cr, double x1, double y1, double x2, double y2) {
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
}

static void path(cairo_t *cr, int count, const double *points, gboolean close) {
    cairo_move_to(cr, points[0], points[1]);
    for (int i = 1; i < count; i++)
        cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
    if (close)
        cairo_close_path(cr);
}

static void lens(cairo_t *cr) {
    cairo_arc(cr, 7, 7, 3.7, 0, 2 * G_PI);
    line(cr, 9.7, 9.7, 13.2, 13.2);
}

// Icons are drawn, not loaded.
static void glyph_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    (void)area;
    Glyph *spec = data;
    double scale = MIN(width, height) / 16.0;
    cairo_translate(cr, (width - 16 * scale) / 2, (height - 16 * scale) / 2);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_rgba(cr, &spec->color);
    cairo_set_line_width(cr, 1.25);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (spec->code) {
    case 1:
        cairo_set_line_width(cr, 2.0);
        line(cr, 3.3, 3.3, 12.7, 12.7);
        line(cr, 12.7, 3.3, 3.3, 12.7);
        break;
    case 0xE8E5:
        path(cr, 5, (double[]){4, 2.5, 9.5, 2.5, 12, 5, 12, 13.5, 4, 13.5}, TRUE);
        path(cr, 3, (double[]){9.5, 2.5, 9.5, 5, 12, 5}, FALSE);
        line(cr, 6, 8, 10, 8);
        line(cr, 6, 10.5, 10, 10.5);
        break;
    case 0xE8B7:
        path(cr, 6, (double[]){2.5, 4, 6.5, 4, 8, 5.7, 13.5, 5.7, 13.5, 12.5, 2.5, 12.5}, TRUE);
        break;
    case 0xE8BB:
    case 0xE711:
        line(cr, 4, 4, 12, 12);
        line(cr, 12, 4, 4, 12);
        break;
    case 0xE80F:
        path(cr, 3, (double[]){2.5, 8, 8, 3, 13.5, 8}, FALSE);
        path(cr, 4, (double[]){4.2, 7.5, 4.2, 13, 11.8, 13, 11.8, 7.5}, FALSE);
        path(cr, 4, (double[]){7, 13, 7, 10, 9, 10, 9, 13}, FALSE);
        break;
    case 0xE8A9:
        cairo_rectangle(cr, 3, 3, 10, 10);
        line(cr, 8, 3, 8, 13);
        line(cr, 3, 6.3, 13, 6.3);
        line(cr, 3, 9.6, 13, 9.6);
        break;
    case 0xE72B:
        line(cr, 13, 8, 3, 8);
        path(cr, 3, (double[]){7, 4, 3, 8, 7, 12}, FALSE);
        break;
    case 0xE72A:
        line(cr, 3, 8, 13, 8);
        path(cr, 3, (double[]){9, 4, 13, 8, 9, 12}, FALSE);
        break;
    case 0xE74A:
        line(cr, 8, 13, 8, 3);
        path(cr, 3, (double[]){4, 7, 8, 3, 12, 7}, FALSE);
        break;
    case 0xE74B:
        line(cr, 8, 3, 8, 13);
        path(cr, 3, (double[]){4, 9, 8, 13, 12, 9}, FALSE);
        break;
    case 0xE721:
        lens(cr);
        break;
    case 0xE8A3:
        lens(cr);
        line(cr, 5.2, 7, 8.8, 7);
        line(cr, 7, 5.2, 7, 8.8);
        break;
    case 0xE71F:
        lens(cr);
        line(cr, 5.2, 7, 8.8, 7);
        break;
    case 0xE790:
        cairo_arc(cr, 8, 8, 5.3, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_arc(cr, 8, 8, 5.3, -G_PI / 2, G_PI / 2);
        cairo_fill(cr);
        break;
    case 0xE9D9:
        cairo_rectangle(cr, 3, 9, 2.2, 4);
        cairo_rectangle(cr, 6.9, 4, 2.2, 9);
        cairo_rectangle(cr, 10.8, 6.5, 2.2, 6.5);
        cairo_fill(cr);
        break;
    case 0xE946:
        cairo_arc(cr, 8, 8, 5.5, 0, 2 * G_PI);
        cairo_stroke(cr);
        line(cr, 8, 7.4, 8, 11);
        cairo_stroke(cr);
        cairo_arc(cr, 8, 5.2, 0.8, 0, 2 * G_PI);
        cairo_fill(cr);
        break;
    case 0xE7C1:
        line(cr, 4, 13.5, 4, 2.8);
        path(cr, 5, (double[]){4, 3.3, 12.5, 3.3, 10.4, 6, 12.5, 8.7, 4, 8.7}, FALSE);
        break;
    case 0xE774:
        cairo_arc(cr, 8, 8, 5.5, 0, 2 * G_PI);
        line(cr, 2.5, 8, 13.5, 8);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, 8, 8);
        cairo_scale(cr, 0.45, 1);
        cairo_arc(cr, 0, 0, 5.5, 0, 2 * G_PI);
        cairo_restore(cr);
        break;
    case 0xE916:
        cairo_arc(cr, 8, 8, 5.5, 0, 2 * G_PI);
        path(cr, 3, (double[]){8, 4.8, 8, 8, 10.4, 9.4}, FALSE);
        break;
    case 0xE7A7:
        path(cr, 3, (double[]){6.7, 3.2, 4, 6, 6.7, 8.8}, FALSE);
        cairo_move_to(cr, 4, 6);
        cairo_line_to(cr, 9.3, 6);
        cairo_arc(cr, 9.3, 9.5, 3.5, -G_PI / 2, G_PI / 2);
        cairo_line_to(cr, 6, 13);
        break;
    case 0xE74E:
        path(cr, 5, (double[]){3, 3, 10.8, 3, 13, 5.2, 13, 13, 3, 13}, TRUE);
        cairo_rectangle(cr, 5.2, 3, 4.6, 3);
        cairo_rectangle(cr, 5.2, 9, 5.6, 4);
        break;
    default:
        line(cr, 3, 4.5, 13, 4.5);
        line(cr, 4.8, 8, 11.2, 8);
        line(cr, 6.6, 11.5, 9.4, 11.5);
        break;
    }
    cairo_stroke(cr);
}

GtkWidget *glyph(guint code, const char *fallback, const char *color) {
    (void)fallback;
    Glyph *spec = g_new0(Glyph, 1);
    spec->code = code;
    gdk_rgba_parse(&spec->color, color ? color : "#bec4c7");
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 19, 19);
    gtk_widget_add_css_class(area, "glyph");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), glyph_draw, spec, g_free);
    return area;
}

GtkWidget *glyph_button(guint code, const char *fallback, const char *color, const char *tip) {
    GtkWidget *button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), glyph(code, fallback, color));
    gtk_widget_add_css_class(button, "tool");
    gtk_widget_set_tooltip_text(button, tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
    gtk_widget_set_focus_on_click(button, FALSE);
    return button;
}

GtkWidget *close_button(const char *tip) {
    GtkWidget *button = gtk_button_new();
    GtkWidget *cross = glyph(1, "", "#e4e7e8");
    gtk_widget_set_size_request(cross, 15, 15);
    gtk_widget_set_halign(cross, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(cross, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(button), cross);
    gtk_widget_add_css_class(button, "closeButton");
    gtk_widget_set_size_request(button, 22, 22);
    gtk_widget_set_tooltip_text(button, tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_focus_on_click(button, FALSE);
    return button;
}

void copy_text(GtkWidget *widget, const char *text) {
    gdk_clipboard_set_text(gtk_widget_get_clipboard(widget), text ? text : "");
}

#ifdef G_OS_WIN32
static LRESULT CALLBACK guard_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC next = (WNDPROC)GetPropW(handle, L"becNext");
    if (message == WM_WINDOWPOSCHANGING && IsZoomed(handle) && !IsIconic(handle)) {
        WINDOWPOS *pos = (WINDOWPOS *)lparam;
        RECT now;
        GetWindowRect(handle, &now);
        if (!(pos->flags & SWP_NOSIZE)) {
            int x = pos->flags & SWP_NOMOVE ? now.left : pos->x, y = pos->flags & SWP_NOMOVE ? now.top : pos->y;
            RECT want = {x, y, x + pos->cx, y + pos->cy};
            MONITORINFO info = {.cbSize = sizeof info};
            GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &info);
            // GTK shrinks maximized windows.
            if (pos->cx < info.rcWork.right - info.rcWork.left || pos->cy < info.rcWork.bottom - info.rcWork.top)
                pos->flags |= SWP_NOSIZE | SWP_NOMOVE;
        }
    }
    if (message == WM_NCDESTROY)
        RemovePropW(handle, L"becNext");
    return CallWindowProcW(next, handle, message, wparam, lparam);
}
#endif

void caption_colors(GtkWindow *window) {
#ifdef G_OS_WIN32
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!surface)
        return;
    HWND handle = gdk_win32_surface_get_handle(surface);
    // Match the reference caption.
    COLORREF caption = 0x008BCCAA, text = 0x00192014;
    DwmSetWindowAttribute(handle, 35, &caption, sizeof caption);
    DwmSetWindowAttribute(handle, 36, &text, sizeof text);
    if (!GetPropW(handle, L"becNext")) {
        SetPropW(handle, L"becNext", (HANDLE)GetWindowLongPtrW(handle, GWLP_WNDPROC));
        SetWindowLongPtrW(handle, GWLP_WNDPROC, (LONG_PTR)guard_proc);
    }
#else
    (void)window;
#endif
}

GtkWidget *pane_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar, "paneBar");
    return bar;
}

static gboolean close_on_escape(GtkWidget *widget, GVariant *args, gpointer user) {
    (void)args;
    (void)user;
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
}

GtkWidget *dialog_window(GtkWindow *parent, const char *title, int width, int height, GtkWidget **content) {
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_window_set_child(GTK_WINDOW(window), box);
    GtkEventController *keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new(close_on_escape, NULL, NULL)));
    gtk_widget_add_controller(window, keys);
    g_signal_connect(window, "realize", G_CALLBACK(caption_colors), NULL);
    *content = box;
    return window;
}

GtkWidget *button_row(GtkWidget *content) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(content), row);
    return row;
}

static gboolean unparent_later(gpointer popover) {
    gtk_widget_unparent(popover);
    return G_SOURCE_REMOVE;
}

static void popover_closed(GtkPopover *popover, gpointer user) {
    (void)user;
    g_idle_add_full(G_PRIORITY_LOW, unparent_later, popover, NULL);
}

void popup_menu(GtkWidget *parent, GMenu *menu, double x, double y) {
    // Anchor menus on the window.
    GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(parent));
    GtkWidget *anchor = root && GTK_IS_WINDOW(root) ? gtk_window_get_child(GTK_WINDOW(root)) : parent;
    graphene_point_t from = GRAPHENE_POINT_INIT((float)x, (float)y), to = from;
    if (!anchor || !gtk_widget_compute_point(parent, anchor, &from, &to)) {
        anchor = parent;
        to = from;
    }
    GtkWidget *popover = gtk_popover_menu_new_from_model_full(G_MENU_MODEL(menu), GTK_POPOVER_MENU_NESTED);
    gtk_widget_set_parent(popover, anchor);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_widget_set_halign(popover, GTK_ALIGN_START);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    GdkRectangle spot = {(int)to.x, (int)to.y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &spot);
    g_signal_connect(popover, "closed", G_CALLBACK(popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}
