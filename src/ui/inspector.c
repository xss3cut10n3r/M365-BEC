#include "ui.h"

#include <string.h>

enum { COL_NAME, COL_VALUE, COL_TIP, COL_NODE, COL_WEIGHT, COL_COUNT };

typedef struct {
    char *label;
    const JV *value;
    char *pointer;
    char *header;
    guint position;
} Source;

typedef struct {
    const JV *value;
    char *path;
    char *pointer;
    gboolean filled, group;
    // Detail rows use these.
    char *field, *text, *filter;
    gint64 related;
} Node;

struct Inspector {
    App *app;
    GtkWidget *paned, *left, *right, *details, *fields, *text, *pages, *source, *mode, *find, *find_status;
    GtkTreeStore *detail_store, *field_store;
    GtkStringList *source_names;
    sqlite3 *db;
    char *last_key, *last_source;
    JDoc *doc;
    GPtrArray *headers;
    GPtrArray *sources;
    GPtrArray *nodes, *detail_nodes;
    GHashTable *expanded;
    gboolean hidden_empty, rendering, updating;
    guint row_width;
};

static void render(Inspector *in);

static void source_free(gpointer data) {
    Source *source = data;
    g_free(source->label);
    g_free(source->pointer);
    g_free(source->header);
    g_free(source);
}

static void node_free(gpointer data) {
    Node *node = data;
    g_free(node->path);
    g_free(node->pointer);
    g_free(node->field);
    g_free(node->text);
    g_free(node->filter);
    g_free(node);
}

static char *show_value(const JV *value) {
    if (!value)
        return g_strdup("null");
    if (value->t == J_OBJ) {
        static const char *const keys[] = {"Subject", "Path", "Name", "DisplayName", "Operation", "Address"};
        for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
            const JV *named = j_get(value, keys[i]);
            if (named && named->t == J_STR && named->n)
                return g_strdup_printf("%s  \xc2\xb7  %u fields", named->u.s, value->n);
        }
        return g_strdup_printf("%u fields", value->n);
    }
    if (value->t == J_ARR)
        return g_strdup_printf(value->n == 1 ? "%u item" : "%u items", value->n);
    return j_scalar(value);
}

static char *serialize(const JV *value) {
    if (!value)
        return g_strdup("null");
    if (value->t == J_STR)
        return g_strdup(value->u.s);
    GString *out = g_string_new(NULL);
    j_dump(out, value, J_INDENT);
    return g_string_free(out, FALSE);
}

static char *source_predicate(const char *pointer, const JV *value) {
    char *quoted = j_quoted(pointer), *out;
    if (j_is_container(value)) {
        out = g_strdup_printf("exists %s", quoted);
    } else {
        char *shown = show_value(value), *literal = j_quoted(shown);
        out = g_strdup_printf("field %s = %s", quoted, literal);
        g_free(shown);
        g_free(literal);
    }
    g_free(quoted);
    return out;
}

static gboolean empty_value(const JV *value) {
    return !value || value->t == J_NULL || (value->t == J_STR && !value->n);
}

static GtkWidget *make_tree(GtkTreeStore **store, const char *first, const char *second) {
    *store = gtk_tree_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(*store));
    const char *titles[] = {first, second};
    for (int i = 0; i < 2; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 1, NULL);
        GtkTreeViewColumn *column =
            gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, i == 0 ? "weight" : NULL, COL_WEIGHT, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, i == 0 ? 225 : 120);
        gtk_tree_view_column_set_expand(column, i == 1);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), COL_TIP);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(view), 0);
    return view;
}

static Node *node_at(GtkTreeModel *model, GtkTreeIter *iter, GPtrArray *nodes) {
    int index = -1;
    gtk_tree_model_get(model, iter, COL_NODE, &index, -1);
    return index >= 0 && (guint)index < nodes->len ? nodes->pdata[index] : NULL;
}

/* Source field tree. */

static gboolean append_node(Inspector *in, GtkTreeIter *parent, GtkTreeIter *out, const char *name, const JV *value,
                            const char *path, const char *pointer) {
    if (in->hidden_empty && empty_value(value))
        return FALSE;
    Node *node = g_new0(Node, 1);
    node->value = value;
    node->path = g_strdup(path);
    node->pointer = g_strdup(pointer);
    g_ptr_array_add(in->nodes, node);
    char *summary = show_value(value);
    GString *brief = g_string_new(summary);
    g_string_replace(brief, "\n", " \xe2\x86\xb5 ", 0);
    if (g_utf8_strlen(brief->str, -1) > 300)
        g_string_truncate(brief, g_utf8_offset_to_pointer(brief->str, 300) - brief->str);
    char *tip = g_utf8_strlen(summary, -1) > 4096 ? g_utf8_substring(summary, 0, 4096) : g_strdup(summary);
    char *escaped = g_markup_escape_text(tip, -1);
    gtk_tree_store_append(in->field_store, out, parent);
    gtk_tree_store_set(in->field_store, out, COL_NAME, name, COL_VALUE, brief->str, COL_TIP, escaped, COL_NODE,
                       (int)in->nodes->len - 1, COL_WEIGHT, 400, -1);
    if (j_is_container(value) && value->n) {
        // Children load on expansion.
        GtkTreeIter dummy;
        gtk_tree_store_append(in->field_store, &dummy, out);
        gtk_tree_store_set(in->field_store, &dummy, COL_NODE, -1, COL_WEIGHT, 400, -1);
    } else {
        node->filled = TRUE;
    }
    g_free(escaped);
    g_free(tip);
    g_string_free(brief, TRUE);
    g_free(summary);
    return TRUE;
}

static void fill_branch(Inspector *in, GtkTreeIter *iter) {
    Node *node = node_at(GTK_TREE_MODEL(in->field_store), iter, in->nodes);
    if (!node || node->filled || node->group)
        return;
    node->filled = TRUE;
    GtkTreeIter dummy;
    gboolean had_dummy = gtk_tree_model_iter_children(GTK_TREE_MODEL(in->field_store), &dummy, iter);
    const JV *value = node->value;
    for (guint i = 0; i < value->n; i++) {
        gboolean list = value->t == J_ARR;
        char *key = list ? g_strdup_printf("%u", i) : g_strdup(value->u.m[i].key);
        const JV *child = list ? value->u.a[i] : value->u.m[i].v;
        char *path = list ? g_strdup_printf("%s[%s]", node->path, key) : g_strdup_printf("%s.%s", node->path, key);
        char *part = list ? g_strdup("*") : escape_pointer(key);
        char *pointer = node->pointer ? g_strdup_printf("%s/%s", node->pointer, part) : NULL;
        GtkTreeIter added;
        append_node(in, iter, &added, key, j_decode(in->doc, child), path, pointer);
        g_free(pointer);
        g_free(part);
        g_free(path);
        g_free(key);
    }
    if (had_dummy)
        gtk_tree_store_remove(in->field_store, &dummy);
}

static gboolean field_test_expand(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path, gpointer user) {
    (void)view;
    (void)path;
    fill_branch(user, iter);
    return FALSE;
}

static void field_expanded(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path, gpointer user) {
    (void)view;
    (void)path;
    Inspector *in = user;
    Node *node = node_at(GTK_TREE_MODEL(in->field_store), iter, in->nodes);
    if (node && !in->rendering)
        g_hash_table_add(in->expanded, g_strdup(node->path));
}

static void field_collapsed(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path, gpointer user) {
    (void)view;
    (void)path;
    Inspector *in = user;
    Node *node = node_at(GTK_TREE_MODEL(in->field_store), iter, in->nodes);
    if (node && !in->rendering)
        g_hash_table_remove(in->expanded, node->path);
}

static void expand_iter(GtkWidget *view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(view), path, FALSE);
    gtk_tree_path_free(path);
}

static void restore_branch(Inspector *in, GtkTreeIter *iter) {
    GtkTreeModel *model = GTK_TREE_MODEL(in->field_store);
    Node *node = node_at(model, iter, in->nodes);
    if (!node)
        return;
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    if (g_hash_table_contains(in->expanded, node->path))
        gtk_tree_view_expand_row(GTK_TREE_VIEW(in->fields), path, FALSE);
    gboolean open = gtk_tree_view_row_expanded(GTK_TREE_VIEW(in->fields), path);
    gtk_tree_path_free(path);
    GtkTreeIter child;
    if (open && gtk_tree_model_iter_children(model, &child, iter)) {
        do
            restore_branch(in, &child);
        while (gtk_tree_model_iter_next(model, &child));
    }
}

// Group columns by evidence type.
static void sections(const char *header, GHashTable *lookup, char *out[3]) {
    out[0] = out[1] = out[2] = NULL;
    char *folded = casefold(header);
    if (g_str_has_prefix(folded, "target") && g_ascii_isdigit(folded[6])) {
        const char *p = folded + 6;
        while (g_ascii_isdigit(*p))
            p++;
        char *number = g_strndup(folded + 6, p - folded - 6);
        out[0] = g_strdup("Directory targets");
        out[1] = g_strdup_printf("Target %s", number);
        if (g_str_has_prefix(p, "modifiedproperty") && g_ascii_isdigit(p[16])) {
            const char *q = p + 16;
            while (g_ascii_isdigit(*q))
                q++;
            if (!strcmp(q, "name") || !strcmp(q, "oldvalue") || !strcmp(q, "newvalue")) {
                char *property = g_strndup(p + 16, q - p - 16);
                char *key = g_strdup_printf("Target%sModifiedProperty%sName", number, property);
                const char *name = g_hash_table_lookup(lookup, key);
                out[2] = name && *name ? g_strdup_printf("Property %s \xc2\xb7 %s", property, name)
                                       : g_strdup_printf("Property %s", property);
                g_free(key);
                g_free(property);
            }
        }
        g_free(number);
        g_free(folded);
        return;
    }
    if (g_str_has_prefix(folded, "additionaldetail") && g_ascii_isdigit(folded[16])) {
        const char *p = folded + 16;
        while (g_ascii_isdigit(*p))
            p++;
        if (!strcmp(p, "key") || !strcmp(p, "value")) {
            char *number = g_strndup(folded + 16, p - folded - 16);
            char *key = g_strdup_printf("AdditionalDetail%sKey", number);
            const char *name = g_hash_table_lookup(lookup, key);
            out[0] = g_strdup("Additional details");
            out[1] = name && *name ? g_strdup(name) : g_strdup_printf("Detail %s", number);
            g_free(key);
            g_free(number);
            g_free(folded);
            return;
        }
    }
    static const char *const identity[] = {"user", "userid", "user id", "username", "user type", "mailbox", NULL};
    static const char *const device[] = {"ip address", "device", "operating system", "browser", "location",
                                         "autonomous system", "client app", "compliant", "managed", "join type",
                                         "user agent", "ipaddress", NULL};
    static const char *const outcome[] = {"auth", "conditional access", "status", "result", "succeeded", "failure",
                                          "credential", "requirement", "error code", NULL};
    static const char *const session[] = {"session", "token", "correlation", "request id", NULL};
    static const char *const resource[] = {"app", "resource", "tenant", "service principal", NULL};
    const char *section = "Event fields";
    if (g_str_has_prefix(folded, "actor") || g_strv_contains(identity, folded)) {
        section = "Identity";
    } else {
        const char *const *groups[] = {device, outcome, session, resource};
        const char *const titles[] = {"Device and network", "Authentication and outcome", "Session and tokens",
                                      "Application and resource"};
        for (int g = 0; g < 4 && !strcmp(section, "Event fields"); g++)
            for (const char *const *word = groups[g]; *word; word++)
                if (strstr(folded, *word)) {
                    section = titles[g];
                    break;
                }
    }
    out[0] = g_strdup(section);
    g_free(folded);
}

static int selected_source(Inspector *in) {
    return (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(in->source)) - 1;
}

static void set_text(Inspector *in, const char *text) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(in->text)), text, -1);
}

static char *row_text(Inspector *in) {
    int index = selected_source(in);
    if (index >= 0 && (guint)index < in->sources->len)
        return serialize(((Source *)in->sources->pdata[index])->value);
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < in->headers->len; i++)
        g_ptr_array_add(cells, g_strdup(in->headers->pdata[i]));
    char *first = csv_line(cells, FALSE);
    g_ptr_array_set_size(cells, 0);
    const JV *row = in->doc ? in->doc->root : NULL;
    for (guint i = 0; row && i < MIN(row->n, in->row_width); i++)
        g_ptr_array_add(cells, j_text(row->u.a[i], TRUE));
    char *second = csv_line(cells, FALSE);
    char *out = g_strconcat(first, second, NULL);
    g_ptr_array_free(cells, TRUE);
    g_free(first);
    g_free(second);
    return out;
}

static void render(Inspector *in) {
    if (in->updating)
        return;
    gtk_tree_store_clear(in->field_store);
    g_ptr_array_set_size(in->nodes, 0);
    gtk_label_set_text(GTK_LABEL(in->find_status), "");
    if (!in->sources->len) {
        set_text(in, "");
        return;
    }
    int selected = selected_source(in);
    guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(in->mode));
    gtk_stack_set_visible_child_name(GTK_STACK(in->pages), mode == 0 ? "tree" : "text");
    if (mode) {
        char *value;
        if (mode == 1) {
            GString *out = g_string_new(NULL);
            if (selected >= 0) {
                const JV *source = ((Source *)in->sources->pdata[selected])->value;
                if (source)
                    j_dump(out, j_decode(in->doc, source), J_INDENT | J_LINE_SEPARATORS);
                else
                    g_string_append(out, "null");
            } else {
                g_string_append(out, "[");
                for (guint i = 0; i < in->sources->len; i++) {
                    Source *source = in->sources->pdata[i];
                    g_string_append(out, i ? ",\n  {\n    \"field\": " : "\n  {\n    \"field\": ");
                    j_quote(out, source->label, -1);
                    g_string_append(out, ",\n    \"value\": ");
                    GString *inner = g_string_new(NULL);
                    if (source->value)
                        j_dump(inner, j_decode(in->doc, source->value), J_INDENT | J_LINE_SEPARATORS);
                    else
                        g_string_append(inner, "null");
                    g_string_replace(inner, "\n", "\n    ", 0);
                    g_string_append(out, inner->str);
                    g_string_free(inner, TRUE);
                    g_string_append(out, "\n  }");
                }
                g_string_append(out, in->sources->len ? "\n]" : "]");
            }
            value = g_string_free(out, FALSE);
        } else {
            value = row_text(in);
        }
        set_text(in, value);
        g_free(value);
        return;
    }
    in->rendering = TRUE;
    GHashTable *lookup = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    for (guint i = 0; i < in->sources->len; i++) {
        Source *source = in->sources->pdata[i];
        g_hash_table_insert(lookup, source->label, source->value ? j_text(source->value, TRUE) : g_strdup(""));
    }
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)gtk_tree_iter_free);
    GPtrArray *order = g_ptr_array_new();
    for (guint i = 0; i < in->sources->len; i++) {
        if (selected >= 0 && (int)i != selected)
            continue;
        Source *source = in->sources->pdata[i];
        if (in->hidden_empty && empty_value(source->value))
            continue;
        GtkTreeIter *parent = NULL;
        if (selected < 0 && in->sources->len > 6) {
            char *names[3];
            sections(source->header ? source->header : source->label, lookup, names);
            GString *prefix = g_string_new(NULL);
            for (int level = 0; level < 3 && names[level]; level++) {
                g_string_append_printf(prefix, "%s%s", level ? " / " : "", names[level]);
                GtkTreeIter *branch = g_hash_table_lookup(groups, prefix->str);
                if (!branch) {
                    GtkTreeIter added;
                    Node *node = g_new0(Node, 1);
                    node->group = node->filled = TRUE;
                    node->path = g_strdup(prefix->str);
                    g_ptr_array_add(in->nodes, node);
                    gtk_tree_store_append(in->field_store, &added, parent);
                    gtk_tree_store_set(in->field_store, &added, COL_NAME, names[level], COL_VALUE, "", COL_NODE,
                                       (int)in->nodes->len - 1, COL_WEIGHT, 400, -1);
                    branch = gtk_tree_iter_copy(&added);
                    char *key = g_strdup(prefix->str);
                    g_hash_table_insert(groups, key, branch);
                    g_ptr_array_add(order, key);
                }
                parent = branch;
            }
            g_string_free(prefix, TRUE);
            for (int level = 0; level < 3; level++)
                g_free(names[level]);
        }
        GtkTreeIter added;
        if (append_node(in, parent, &added, source->label, j_decode(in->doc, source->value), source->label, source->pointer) &&
            selected >= 0) {
            fill_branch(in, &added);
            expand_iter(in->fields, GTK_TREE_MODEL(in->field_store), &added);
        }
    }
    for (guint i = 0; i < order->len; i++) {
        const char *key = order->pdata[i];
        GtkTreeIter *branch = g_hash_table_lookup(groups, key);
        char *count = g_strdup_printf("%d entries", gtk_tree_model_iter_n_children(GTK_TREE_MODEL(in->field_store), branch));
        gtk_tree_store_set(in->field_store, branch, COL_VALUE, count, -1);
        g_free(count);
        if (!strcmp(key, "Event fields") || g_hash_table_contains(in->expanded, key))
            expand_iter(in->fields, GTK_TREE_MODEL(in->field_store), branch);
    }
    GtkTreeIter top;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(in->field_store), &top)) {
        do
            restore_branch(in, &top);
        while (gtk_tree_model_iter_next(GTK_TREE_MODEL(in->field_store), &top));
    }
    g_ptr_array_free(order, TRUE);
    g_hash_table_destroy(groups);
    g_hash_table_destroy(lookup);
    in->rendering = FALSE;
}

void inspector_clear(Inspector *in) {
    g_clear_pointer(&in->last_key, g_free);
    g_clear_pointer(&in->last_source, g_free);
    g_ptr_array_set_size(in->headers, 0);
    g_ptr_array_set_size(in->sources, 0);
    gtk_tree_store_clear(in->detail_store);
    gtk_tree_store_clear(in->field_store);
    g_ptr_array_set_size(in->nodes, 0);
    g_ptr_array_set_size(in->detail_nodes, 0);
    in->updating = TRUE;
    gtk_string_list_splice(in->source_names, 0, g_list_model_get_n_items(G_LIST_MODEL(in->source_names)), NULL);
    in->updating = FALSE;
    set_text(in, "");
    gtk_label_set_text(GTK_LABEL(in->find_status), "");
    g_clear_pointer(&in->doc, j_free);
}

/* Event detail tree. */

static void detail_group(Inspector *in, GtkTreeIter *iter, const char *name, const char *summary, const char *field,
                         const char *filter) {
    Node *node = g_new0(Node, 1);
    node->group = TRUE;
    node->field = g_strdup(field);
    node->text = g_strdup(summary);
    node->filter = g_strdup(filter);
    g_ptr_array_add(in->detail_nodes, node);
    char *tip = g_markup_escape_text(summary, -1);
    gtk_tree_store_append(in->detail_store, iter, NULL);
    gtk_tree_store_set(in->detail_store, iter, COL_NAME, name, COL_VALUE, summary, COL_TIP, tip, COL_NODE,
                       (int)in->detail_nodes->len - 1, COL_WEIGHT, 700, -1);
    g_free(tip);
}

static Node *detail_leaf(Inspector *in, GtkTreeIter *parent, const char *name, const char *value, const char *field) {
    if (!value || !*value)
        return NULL;
    Node *node = g_new0(Node, 1);
    node->field = g_strdup(field);
    node->text = g_strdup(value);
    g_ptr_array_add(in->detail_nodes, node);
    char *tip = g_markup_escape_text(value, -1);
    GtkTreeIter iter;
    gtk_tree_store_append(in->detail_store, &iter, parent);
    gtk_tree_store_set(in->detail_store, &iter, COL_NAME, name, COL_VALUE, value, COL_TIP, tip, COL_NODE,
                       (int)in->detail_nodes->len - 1, COL_WEIGHT, 400, -1);
    g_free(tip);
    return node;
}

static char *row_predicate(const char *path, const char *digest, const char *sheet, gint64 rownum) {
    char *a = j_quoted(path), *b = j_quoted(digest), *c = j_quoted(sheet);
    char *out = g_strdup_printf("path = %s AND sha256 = %s AND sheet = %s AND row = %" G_GINT64_FORMAT, a, b, c, rownum);
    g_free(a);
    g_free(b);
    g_free(c);
    return out;
}

static gboolean occurrence_prefix(const char *header) {
    if (header[0] != '@' || header[1] < '1' || header[1] > '9')
        return FALSE;
    const char *p = header + 1;
    while (g_ascii_isdigit(*p))
        p++;
    return *p == ':';
}

void inspector_set(Inspector *in, sqlite3 *db, const Record *record) {
    char *key = g_strdup_printf("%" G_GINT64_FORMAT "|%s|%s|%" G_GINT64_FORMAT, record->id, record->reason, record->verdict,
                                record->number);
    if (in->last_key && !strcmp(in->last_key, key)) {
        g_free(key);
        return;
    }
    sqlite3_stmt *r = sql_prepare(db,
                                  "SELECT e.rownum,e.time,e.actor,e.op,e.result,e.request,e.ip,e.raw,e.issue,l.kind,l.headers,"
                                  "l.name,l.sheet,f.digest,f.path FROM events e JOIN logs l ON e.log=l.id "
                                  "JOIN files f ON l.file=f.id WHERE e.id=?",
                                  NULL);
    if (r)
        sqlite3_bind_int64(r, 1, record->id);
    if (!r || sqlite3_step(r) != SQLITE_ROW) {
        sqlite3_finalize(r);
        g_free(key);
        inspector_clear(in);
        return;
    }
    in->db = db;
    GtkStringObject *shown = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(in->source));
    char *preferred = in->last_source && !strcmp(in->last_source, record->source) && shown
                          ? g_strdup(gtk_string_object_get_string(shown)) : NULL;
    g_free(in->last_source);
    in->last_source = g_strdup(record->source);
    // Remember open detail groups.
    GHashTable *open_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GtkTreeIter top;
    GtkTreeModel *details = GTK_TREE_MODEL(in->detail_store);
    if (gtk_tree_model_get_iter_first(details, &top)) {
        do {
            GtkTreePath *path = gtk_tree_model_get_path(details, &top);
            if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(in->details), path)) {
                char *name;
                gtk_tree_model_get(details, &top, COL_NAME, &name, -1);
                g_hash_table_add(open_groups, name);
            }
            gtk_tree_path_free(path);
        } while (gtk_tree_model_iter_next(details, &top));
    }
    gboolean first = in->last_key == NULL;
    g_free(in->last_key);
    in->last_key = key;
    gint64 rownum = sqlite3_column_int64(r, 0);
    const char *time = column_text(r, 1), *actor = column_text(r, 2), *op = column_text(r, 3);
    const char *result = column_text(r, 4), *request = column_text(r, 5), *ip = column_text(r, 6);
    const char *raw = column_text(r, 7), *issue = column_text(r, 8), *kind = column_text(r, 9);
    const char *headers = column_text(r, 10), *name = column_text(r, 11), *sheet = column_text(r, 12);
    const char *digest = column_text(r, 13), *path = column_text(r, 14);
    gboolean json = log_is_json(name);
    g_ptr_array_set_size(in->headers, 0);
    g_ptr_array_set_size(in->sources, 0);
    j_free(in->doc);
    in->doc = j_parse(raw, -1, 0, NULL);
    Columns *columns = columns_new(headers);
    for (guint i = 0; i < columns->names->len; i++)
        g_ptr_array_add(in->headers, g_strdup(columns->names->pdata[i]));
    const JV *row = in->doc && in->doc->root->t == J_ARR ? in->doc->root : NULL;
    guint width = in->headers->len, values = row ? row->n : 0;
    const JV *present = row && values == width + 1 && row->u.a[width]->t == J_ARR ? row->u.a[width] : NULL;
    if (present)
        values = width;
    in->row_width = values;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < MAX(width, values); i++) {
        char *header = i < width ? g_strdup(in->headers->pdata[i]) : g_strdup_printf("Extra column %u", i + 1);
        char *clean = clean_header(header);
        GArray *positions = g_hash_table_lookup(columns->positions, clean);
        guint total = i < width && positions ? positions->len : 1;
        guint ordinal = GPOINTER_TO_UINT(g_hash_table_lookup(seen, clean)) + 1;
        g_hash_table_insert(seen, g_strdup(clean), GUINT_TO_POINTER(ordinal));
        gboolean there = i < values;
        if (present) {
            there = FALSE;
            for (guint k = 0; k < present->n && !there; k++)
                there = (guint)atoi(present->u.a[k]->u.s) == i;
        }
        if (json && !there) {
            g_free(header);
            g_free(clean);
            continue;
        }
        Source *source = g_new0(Source, 1);
        source->label = total > 1 ? g_strdup_printf("%s [%u]", header, ordinal) : g_strdup(header);
        source->header = i < width ? g_strdup(header) : NULL;
        source->value = i < values ? row->u.a[i] : NULL;
        source->position = i;
        if (i < MIN(width, values)) {
            char *head = total > 1 || occurrence_prefix(header) ? g_strdup_printf("@%u:%s", ordinal, header) : g_strdup(header);
            char *escaped = escape_pointer(head);
            source->pointer = g_strconcat("/", escaped, NULL);
            g_free(escaped);
            g_free(head);
        }
        g_ptr_array_add(in->sources, source);
        g_free(header);
        g_free(clean);
    }
    g_hash_table_destroy(seen);
    in->updating = TRUE;
    gtk_string_list_splice(in->source_names, 0, g_list_model_get_n_items(G_LIST_MODEL(in->source_names)), NULL);
    gtk_string_list_append(in->source_names, "Imported row");
    guint choice = 0;
    for (guint i = 0; i < in->sources->len; i++) {
        Source *source = in->sources->pdata[i];
        gtk_string_list_append(in->source_names, source->label);
        if (preferred ? !strcmp(preferred, source->label) : (!choice && !strcmp(source->label, "AuditData")))
            choice = i + 1;
    }
    if (preferred && !choice)
        for (guint i = 0; i < in->sources->len; i++)
            if (!strcmp(((Source *)in->sources->pdata[i])->label, "AuditData"))
                choice = i + 1;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(in->source), choice);
    in->updating = FALSE;
    g_free(preferred);
    gtk_tree_store_clear(in->detail_store);
    g_ptr_array_set_size(in->detail_nodes, 0);
    GPtrArray *expand = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_tree_iter_free);
#define GROUP(iter, title, summary, field, filter)                                                            \
    do {                                                                                                      \
        detail_group(in, &iter, title, summary, field, filter);                                               \
        if (g_hash_table_contains(open_groups, title) || (first && !strcmp(title, "Event")))                 \
            g_ptr_array_add(expand, gtk_tree_iter_copy(&iter));                                               \
    } while (0)
    GtkTreeIter group;
    GROUP(group, "Event", op, "operation", NULL);
    char *number = g_strdup_printf("%" G_GINT64_FORMAT, record->number);
    detail_leaf(in, &group, "ID", number, "id");
    g_free(number);
    detail_leaf(in, &group, "Time (UTC)", time, "time");
    detail_leaf(in, &group, "Actor", actor, "actor");
    detail_leaf(in, &group, "Operation", op, "operation");
    detail_leaf(in, &group, "Result", result, "result");
    detail_leaf(in, &group, "Source", record->source, "source");
    detail_leaf(in, &group, "Verdict", record->verdict, "verdict");
    detail_leaf(in, &group, "Verdict scope", record->reason, "markreason");
    detail_leaf(in, &group, "Import warning", issue, "issue");
    EventFields *meta = event_fields_new(headers, raw);
    static const struct {
        const char *title;
        const char *pairs[8][2];
    } blocks[] = {
        {"Identity and network",
         {{"IP address", "ip"}, {"Device ID", "device"}, {"Session ID", "session"}, {"Location", "location"},
          {"Operating system", "os"}, {"Request ID", "request"}, {"Correlation ID", "correlation"}}},
        {"Authentication and application",
         {{"Application", "application"}, {"Application ID", "appid"}, {"Client", "client"},
          {"Authentication method", "authmethod"}, {"Conditional Access", "conditionalaccess"},
          {"Service principal", "serviceprincipal"}}},
        {"Mailbox and directory",
         {{"Workload", "workload"}, {"Recipient", "recipient"}, {"Subject", "subject"}, {"Message ID", "messageid"},
          {"Rule", "rule"}, {"Mail access", "access"}, {"Folder", "folder"}, {"Directory target", "target"}}},
    };
    for (guint b = 0; b < G_N_ELEMENTS(blocks); b++) {
        GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
        for (int p = 0; p < 8 && blocks[b].pairs[p][0]; p++) {
            const char *field = blocks[b].pairs[p][1];
            GPtrArray *found;
            if (!strcmp(field, "ip")) {
                found = g_ptr_array_new_with_free_func(g_free);
                char **parts = g_strsplit(ip, ", ", -1);
                for (char **part = parts; *part; part++)
                    g_ptr_array_add(found, g_strdup(*part));
                g_strfreev(parts);
            } else if (!strcmp(field, "request")) {
                found = g_ptr_array_new_with_free_func(g_free);
                g_ptr_array_add(found, g_strdup(request));
            } else {
                found = event_values(meta, field);
            }
            for (guint i = 0; i < found->len; i++) {
                if (!*(char *)found->pdata[i])
                    continue;
                g_ptr_array_add(entries, g_strdup(blocks[b].pairs[p][0]));
                g_ptr_array_add(entries, g_strdup(found->pdata[i]));
                g_ptr_array_add(entries, g_strdup(field));
            }
            g_ptr_array_free(found, TRUE);
        }
        if (entries->len) {
            GROUP(group, blocks[b].title, entries->pdata[1], entries->pdata[2], NULL);
            for (guint i = 0; i + 2 < entries->len; i += 3)
                detail_leaf(in, &group, entries->pdata[i], entries->pdata[i + 1], entries->pdata[i + 2]);
        }
        g_ptr_array_free(entries, TRUE);
    }
    event_fields_free(meta);
    char *location = source_location(name, sheet, rownum);
    char *predicate = row_predicate(path, digest, sheet, rownum);
    GROUP(group, "Evidence location", location, NULL, predicate);
    g_free(predicate);
    g_free(location);
    detail_leaf(in, &group, "File", name, "file");
    char *row_text_value = g_strdup_printf("%" G_GINT64_FORMAT, rownum);
    if (json) {
        char *pointer = json_path(sheet, rownum);
        detail_leaf(in, &group, "JSON path", pointer, NULL);
        detail_leaf(in, &group, "Record", row_text_value, "row");
        g_free(pointer);
    } else {
        detail_leaf(in, &group, "Sheet", sheet, "sheet");
        detail_leaf(in, &group, "Row", row_text_value, "row");
    }
    g_free(row_text_value);
    detail_leaf(in, &group, "Original path", path, "path");
    detail_leaf(in, &group, "SHA-256", digest, "sha256");
    if (*request && (g_str_has_prefix(kind, "Interactive ") || g_str_has_prefix(kind, "Non-interactive "))) {
        char *family = g_strndup(kind, strchr(kind, ' ') - kind);
        char *target = g_strconcat(family, g_str_has_suffix(kind, "details") ? " sign-ins" : " auth details", NULL);
        sqlite3_stmt *links = sql_prepare(db,
                                          "SELECT e.id,e.rownum,l.name,l.sheet,f.path,f.digest,e.op,e.result FROM events e "
                                          "JOIN logs l ON l.id=e.log JOIN files f ON f.id=l.file "
                                          "WHERE e.request!='' AND e.request=? AND l.kind=? ORDER BY e.time,e.id",
                                          NULL);
        GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
        if (links) {
            sqlite3_bind_text(links, 1, request, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(links, 2, target, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(links) == SQLITE_ROW) {
                g_ptr_array_add(rows, g_strdup_printf("%" G_GINT64_FORMAT, (gint64)sqlite3_column_int64(links, 0)));
                g_ptr_array_add(rows, g_strdup_printf("Row %" G_GINT64_FORMAT, (gint64)sqlite3_column_int64(links, 1)));
                g_ptr_array_add(rows, g_strdup_printf("%s \xc2\xb7 %s", column_text(links, 6), column_text(links, 7)));
                g_ptr_array_add(rows, row_predicate(column_text(links, 4), column_text(links, 5), column_text(links, 3),
                                                    sqlite3_column_int64(links, 1)));
            }
            sqlite3_finalize(links);
        }
        char *summary = g_strdup_printf("%u matching", rows->len / 4);
        char *quoted = j_quoted(request);
        char *filter = g_strdup_printf("request = %s", quoted);
        GROUP(group, "Related authentication records", summary, NULL, filter);
        for (guint i = 0; i + 3 < rows->len; i += 4) {
            Node *leaf = detail_leaf(in, &group, rows->pdata[i + 1], rows->pdata[i + 2], NULL);
            if (leaf) {
                leaf->related = g_ascii_strtoll(rows->pdata[i], NULL, 10);
                leaf->filter = g_strdup(rows->pdata[i + 3]);
            }
        }
        g_free(filter);
        g_free(quoted);
        g_free(summary);
        g_ptr_array_free(rows, TRUE);
        g_free(target);
        g_free(family);
    }
#undef GROUP
    for (guint i = 0; i < expand->len; i++)
        expand_iter(in->details, details, expand->pdata[i]);
    g_ptr_array_free(expand, TRUE);
    g_hash_table_destroy(open_groups);
    columns_free(columns);
    sqlite3_finalize(r);
    render(in);
}

/* Find, context menus, selection. */

static void collect_matches(Inspector *in, GtkTreeIter *iter, const char *term, GPtrArray *matches) {
    GtkTreeModel *model = GTK_TREE_MODEL(in->field_store);
    Node *node = node_at(model, iter, in->nodes);
    if (!node)
        return;
    char *name;
    gtk_tree_model_get(model, iter, COL_NAME, &name, -1);
    gboolean hit = contains_folded(name, term);
    if (!hit && node->value && !j_is_container(node->value)) {
        char *text = j_text(node->value, FALSE);
        hit = contains_folded(text, term);
        g_free(text);
    }
    g_free(name);
    if (hit)
        g_ptr_array_add(matches, gtk_tree_model_get_path(model, iter));
    fill_branch(in, iter);
    GtkTreeIter child;
    if (gtk_tree_model_iter_children(model, &child, iter)) {
        do
            collect_matches(in, &child, term, matches);
        while (gtk_tree_model_iter_next(model, &child));
    }
}

static void find_next(Inspector *in) {
    const char *wanted = gtk_editable_get_text(GTK_EDITABLE(in->find));
    if (!*wanted)
        return;
    if (gtk_drop_down_get_selected(GTK_DROP_DOWN(in->mode)) != 0) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(in->text));
        GtkTextIter start, from, to;
        gtk_text_buffer_get_selection_bounds(buffer, NULL, &start);
        gboolean found = gtk_text_iter_forward_search(&start, wanted, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &from, &to, NULL);
        if (!found) {
            gtk_text_buffer_get_start_iter(buffer, &start);
            found = gtk_text_iter_forward_search(&start, wanted, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &from, &to, NULL);
        }
        gtk_label_set_text(GTK_LABEL(in->find_status), found ? "" : "No match");
        if (found) {
            gtk_text_buffer_select_range(buffer, &from, &to);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(in->text), &from, 0.1, FALSE, 0, 0);
        }
        return;
    }
    char *term = casefold(wanted);
    GPtrArray *matches = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_tree_path_free);
    GtkTreeModel *model = GTK_TREE_MODEL(in->field_store);
    in->rendering = TRUE;
    GtkTreeIter top;
    if (gtk_tree_model_get_iter_first(model, &top)) {
        do
            collect_matches(in, &top, term, matches);
        while (gtk_tree_model_iter_next(model, &top));
    }
    g_free(term);
    if (!matches->len) {
        gtk_label_set_text(GTK_LABEL(in->find_status), "No match");
    } else {
        GtkTreePath *current = NULL;
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(in->fields), &current, NULL);
        guint index = 0;
        for (guint i = 0; current && i < matches->len; i++)
            if (!gtk_tree_path_compare(current, matches->pdata[i]))
                index = (i + 1) % matches->len;
        if (current)
            gtk_tree_path_free(current);
        GtkTreePath *hit = matches->pdata[index];
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(in->fields), hit);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(in->fields), hit, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(in->fields), hit, NULL, FALSE, 0, 0);
        char *status = g_strdup_printf("%u/%u", index + 1, matches->len);
        gtk_label_set_text(GTK_LABEL(in->find_status), status);
        g_free(status);
    }
    in->rendering = FALSE;
    g_ptr_array_free(matches, TRUE);
}

static void find_activated(GtkWidget *widget, gpointer user) {
    (void)widget;
    find_next(user);
}

static void tree_context(GtkGestureClick *gesture, int presses, double x, double y, gpointer user) {
    (void)presses;
    Inspector *in = user;
    GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    gboolean is_fields = view == in->fields;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    int bx, by;
    gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(view), (int)x, (int)y, &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), bx, by, &path, NULL, NULL, NULL))
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    in->updating = TRUE;
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, NULL, FALSE);
    in->updating = FALSE;
    GtkTreeIter iter;
    gtk_tree_model_get_iter(model, &iter, path);
    Node *node = node_at(model, &iter, is_fields ? in->nodes : in->detail_nodes);
    GMenu *menu = g_menu_new(), *filters = g_menu_new(), *tools = g_menu_new();
    if (node && is_fields) {
        if (node->pointer) {
            char *predicate = source_predicate(node->pointer, node->value);
            filter_menu_items(in->app, filters, predicate);
            g_free(predicate);
            if (!j_is_container(node->value)) {
                char *name;
                gtk_tree_model_get(model, &iter, COL_NAME, &name, -1);
                char *target = g_strdup_printf("%s\x1f%s", name, node->pointer);
                add_text_action(filters, "Apply as column", "win.add-column", target);
                g_free(target);
                g_free(name);
            }
            add_text_action(tools, "Copy BQL field path", "win.copy-text", node->pointer);
        }
        if (!node->group) {
            add_text_action(tools, "Copy source location", "win.copy-text", node->path);
            char *text = serialize(node->value);
            add_text_action(tools, "Copy value", "win.copy-text", text);
            g_free(text);
        }
    } else if (node) {
        char *expression = g_strdup(node->filter);
        if (node->field && node->text && !node->related) {
            g_free(expression);
            char *quoted = j_quoted(node->text);
            expression = g_strdup_printf("%s = %s", node->field, quoted);
            g_free(quoted);
        }
        if (expression)
            filter_menu_items(in->app, filters, expression);
        g_free(expression);
        if (node->text)
            add_text_action(tools, "Copy value", "win.copy-text", node->text);
        if (node->related) {
            char *id = g_strdup_printf("%" G_GINT64_FORMAT, node->related);
            add_text_action(tools, "Go to related event", "win.follow-event", id);
            g_free(id);
        }
    }
    if (gtk_tree_model_iter_has_child(model, &iter)) {
        char *spot = gtk_tree_path_to_string(path);
        char *target = g_strdup_printf("%s\x1f%s", is_fields ? "fields" : "details", spot);
        add_text_action(tools, gtk_tree_view_row_expanded(GTK_TREE_VIEW(view), path) ? "Collapse branch" : "Expand branch",
                        "inspector.toggle", target);
        g_free(target);
        g_free(spot);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(filters));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tools));
    if (g_menu_model_get_n_items(G_MENU_MODEL(filters)) + g_menu_model_get_n_items(G_MENU_MODEL(tools)))
        popup_menu(view, menu, x, y);
    g_object_unref(filters);
    g_object_unref(tools);
    g_object_unref(menu);
    gtk_tree_path_free(path);
}

typedef struct {
    char *pointer;
    const JV *value;
} Target;

// Map JSON lines to fields.
static void line_targets(Inspector *in, GArray *out, const JV *value, const char *pointer, guint limit) {
    if (out->len > limit)
        return;
    Target target = {g_strdup(pointer), value};
    g_array_append_val(out, target);
    if (!j_is_container(value) || !value->n)
        return;
    for (guint i = 0; i < value->n && out->len <= limit; i++) {
        gboolean list = value->t == J_ARR;
        char *part = list ? g_strdup("*") : escape_pointer(value->u.m[i].key);
        char *path = pointer ? g_strdup_printf("%s/%s", pointer, part) : NULL;
        line_targets(in, out, list ? value->u.a[i] : value->u.m[i].v, path, limit);
        g_free(path);
        g_free(part);
    }
    Target closing = {g_strdup(pointer), value};
    g_array_append_val(out, closing);
}

static char *text_predicate(Inspector *in, GtkTextIter *spot) {
    if (!in->sources->len)
        return NULL;
    guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(in->mode));
    int index = selected_source(in);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(in->text));
    if (mode == 1) {
        guint line = (guint)gtk_text_iter_get_line(spot);
        GArray *targets = g_array_new(FALSE, FALSE, sizeof(Target));
        Target none = {NULL, NULL};
        if (index >= 0) {
            Source *source = in->sources->pdata[index];
            line_targets(in, targets, j_decode(in->doc, source->value), source->pointer, line);
        } else {
            // Map wrapper lines to columns.
            g_array_append_val(targets, none);
            for (guint i = 0; i < in->sources->len && targets->len <= line; i++) {
                Source *source = in->sources->pdata[i];
                const JV *value = j_decode(in->doc, source->value);
                for (int k = 0; k < 2; k++) {
                    Target wrapper = {g_strdup(source->pointer), value};
                    g_array_append_val(targets, wrapper);
                }
                line_targets(in, targets, value, source->pointer, line);
                Target wrapper = {g_strdup(source->pointer), value};
                g_array_append_val(targets, wrapper);
            }
        }
        char *out = NULL;
        if (line < targets->len) {
            Target *target = &g_array_index(targets, Target, line);
            if (target->pointer && target->value)
                out = source_predicate(target->pointer, target->value);
        }
        for (guint i = 0; i < targets->len; i++)
            g_free(g_array_index(targets, Target, i).pointer);
        g_array_free(targets, TRUE);
        return out;
    }
    if (index < 0) {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(buffer, &start);
        char *prefix = gtk_text_buffer_get_text(buffer, &start, spot, FALSE);
        int row = 0, column = 0;
        gboolean quoted = FALSE;
        for (const char *p = prefix; *p; p++) {
            if (*p == '"') {
                quoted = !quoted;
            } else if (!quoted && *p == ',') {
                column++;
            } else if (!quoted && *p == '\n') {
                row++;
                column = 0;
            }
        }
        g_free(prefix);
        // Cells keep original header positions.
        Source *source = NULL;
        for (guint i = 0; i < in->sources->len; i++) {
            Source *candidate = in->sources->pdata[i];
            if (candidate->pointer && candidate->position == (guint)column) {
                source = candidate;
                index = (int)i;
            }
        }
        if (row > 1 || !source)
            return NULL;
        if (row == 0) {
            char *quoted_pointer = j_quoted(source->pointer);
            char *out = g_strdup_printf("exists %s", quoted_pointer);
            g_free(quoted_pointer);
            return out;
        }
    }
    Source *source = in->sources->pdata[index];
    if (!source->pointer || !source->value)
        return NULL;
    GtkTextIter from, to;
    if (gtk_text_buffer_get_selection_bounds(buffer, &from, &to) && gtk_text_iter_compare(&from, spot) <= 0 &&
        gtk_text_iter_compare(spot, &to) < 0 && source->value->t == J_STR) {
        char *fragment = gtk_text_buffer_get_text(buffer, &from, &to, FALSE);
        if (*fragment && strstr(source->value->u.s, fragment)) {
            char *a = j_quoted(source->pointer), *b = j_quoted(fragment);
            char *out = g_strdup_printf("field %s contains %s", a, b);
            g_free(a);
            g_free(b);
            g_free(fragment);
            return out;
        }
        g_free(fragment);
    }
    return source_predicate(source->pointer, j_decode(in->doc, source->value));
}

static void text_pressed(GtkGestureClick *gesture, int presses, double x, double y, gpointer user) {
    (void)gesture;
    (void)presses;
    Inspector *in = user;
    int bx, by;
    GtkTextIter spot;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(in->text), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(in->text), &spot, bx, by);
    char *expression = text_predicate(in, &spot);
    GMenu *menu = g_menu_new();
    if (expression)
        filter_menu_items(in->app, menu, expression);
    gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(in->text), G_MENU_MODEL(menu));
    g_object_unref(menu);
    g_free(expression);
}

static void detail_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user) {
    (void)column;
    Inspector *in = user;
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    Node *node = node_at(model, &iter, in->detail_nodes);
    if (node && node->related) {
        char *id = g_strdup_printf("%" G_GINT64_FORMAT, node->related);
        gtk_widget_activate_action(GTK_WIDGET(view), "win.follow-event", "s", id);
        g_free(id);
    }
}

// Select the original cell.
static void detail_selected(GtkTreeView *view, gpointer user) {
    Inspector *in = user;
    if (in->updating)
        return;
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(view, &path, NULL);
    if (!path)
        return;
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    Node *node = gtk_tree_model_get_iter(model, &iter, path) ? node_at(model, &iter, in->detail_nodes) : NULL;
    gtk_tree_path_free(path);
    if (!node || !node->field || !node->text || node->group)
        return;
    static const char *const extra[][5] = {
        {"ip", "IP address", "IPAddress"},
        {"actor", "Username", "UserId", "ActorDisplayName", "ActorUserPrincipalName"},
        {"time", "Date (UTC)", "Date", "CreationDate"},
        {"operation", "Operation", "Activity"},
        {"result", "Status", "Result", "Succeeded"},
        {"request", "Request ID"},
    };
    GPtrArray *expected = g_ptr_array_new();
    const Slot *slot = slot_find(node->field);
    for (int i = 0; slot && i < 5 && slot->outer[i]; i++)
        g_ptr_array_add(expected, (gpointer)slot->outer[i]);
    for (guint i = 0; i < G_N_ELEMENTS(extra); i++)
        if (!strcmp(extra[i][0], node->field))
            for (int k = 1; k < 5 && extra[i][k]; k++)
                g_ptr_array_add(expected, (gpointer)extra[i][k]);
    char *wanted = casefold(node->text);
    for (guint i = 0; i < in->sources->len; i++) {
        Source *source = in->sources->pdata[i];
        gboolean named = FALSE;
        for (guint k = 0; k < expected->len && !named; k++)
            named = !strcmp(expected->pdata[k], source->label);
        if (!named || !source->value)
            continue;
        char *text = j_text(source->value, FALSE);
        gboolean same = equals_folded(text, wanted);
        g_free(text);
        if (same) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(in->source), i + 1);
            break;
        }
    }
    g_free(wanted);
    g_ptr_array_free(expected, TRUE);
}

static void selection_notify(GObject *object, GParamSpec *spec, gpointer user) {
    (void)object;
    (void)spec;
    render(user);
}

static gboolean tree_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer user) {
    (void)code;
    (void)user;
    if (key != GDK_KEY_Return && key != GDK_KEY_KP_Enter && key != GDK_KEY_ISO_Enter)
        return FALSE;
    if (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK))
        return FALSE;
    GtkTreeView *view = GTK_TREE_VIEW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(view, &path, NULL);
    GtkTreeIter iter;
    gboolean branch = path && gtk_tree_model_get_iter(gtk_tree_view_get_model(view), &iter, path) &&
                      gtk_tree_model_iter_has_child(gtk_tree_view_get_model(view), &iter);
    // Enter toggles like double-click.
    if (branch && gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else if (branch)
        gtk_tree_view_expand_row(view, path, FALSE);
    if (path)
        gtk_tree_path_free(path);
    return branch;
}

static void toggle_branch(GSimpleAction *action, GVariant *parameter, gpointer user) {
    (void)action;
    Inspector *in = user;
    char **parts = g_strsplit(g_variant_get_string(parameter, NULL), "\x1f", 2);
    GtkWidget *view = !strcmp(parts[0], "fields") ? in->fields : in->details;
    GtkTreePath *path = gtk_tree_path_new_from_string(parts[1]);
    if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(view), path))
        gtk_tree_view_collapse_row(GTK_TREE_VIEW(view), path);
    else
        gtk_tree_view_expand_row(GTK_TREE_VIEW(view), path, FALSE);
    gtk_tree_path_free(path);
    g_strfreev(parts);
}

static void simple_action(GSimpleAction *action, GVariant *parameter, gpointer user) {
    (void)parameter;
    Inspector *in = user;
    const char *name = g_action_get_name(G_ACTION(action));
    if (!strcmp(name, "expand")) {
        // Expand two loaded levels.
        GtkTreeModel *model = GTK_TREE_MODEL(in->field_store);
        GtkTreeIter top, child;
        if (gtk_tree_model_get_iter_first(model, &top)) {
            do {
                expand_iter(in->fields, model, &top);
                if (gtk_tree_model_iter_children(model, &child, &top)) {
                    do
                        expand_iter(in->fields, model, &child);
                    while (gtk_tree_model_iter_next(model, &child));
                }
            } while (gtk_tree_model_iter_next(model, &top));
        }
    } else if (!strcmp(name, "collapse")) {
        gtk_tree_view_collapse_all(GTK_TREE_VIEW(in->fields));
    } else if (!strcmp(name, "copy")) {
        char *text;
        if (gtk_drop_down_get_selected(GTK_DROP_DOWN(in->mode)) != 0) {
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(in->text));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        } else {
            text = in->sources->len ? row_text(in) : g_strdup("");
        }
        copy_text(in->paned, text);
        g_free(text);
    } else if (!strcmp(name, "expand-details")) {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(in->details));
    } else if (!strcmp(name, "collapse-details")) {
        gtk_tree_view_collapse_all(GTK_TREE_VIEW(in->details));
    }
}

static void toggle_state(GSimpleAction *action, GVariant *state, gpointer user) {
    Inspector *in = user;
    g_simple_action_set_state(action, state);
    gboolean on = g_variant_get_boolean(state);
    if (!strcmp(g_action_get_name(G_ACTION(action)), "hide-empty")) {
        in->hidden_empty = on;
        render(in);
    } else {
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(in->text), on ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    }
}

static void close_pane(GtkButton *button, gpointer user) {
    app_set_pane(user, g_object_get_data(G_OBJECT(button), "pane"), FALSE);
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), child);
    gtk_widget_set_vexpand(scroller, TRUE);
    return scroller;
}

static GtkWidget *text_button(const char *text, const char *tip, const char *action) {
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(button, "tool");
    gtk_widget_set_tooltip_text(button, tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

Inspector *inspector_new(App *app) {
    Inspector *in = g_new0(Inspector, 1);
    in->app = app;
    in->headers = g_ptr_array_new_with_free_func(g_free);
    in->sources = g_ptr_array_new_with_free_func(source_free);
    in->nodes = g_ptr_array_new_with_free_func(node_free);
    in->detail_nodes = g_ptr_array_new_with_free_func(node_free);
    in->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    in->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_shrink_start_child(GTK_PANED(in->paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(in->paned), FALSE);
    GSimpleActionGroup *actions = g_simple_action_group_new();
    static const char *const simple[] = {"expand", "collapse", "copy", "expand-details", "collapse-details"};
    for (guint i = 0; i < G_N_ELEMENTS(simple); i++) {
        GSimpleAction *action = g_simple_action_new(simple[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(simple_action), in);
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
        g_object_unref(action);
    }
    GSimpleAction *toggle = g_simple_action_new("toggle", G_VARIANT_TYPE_STRING);
    g_signal_connect(toggle, "activate", G_CALLBACK(toggle_branch), in);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(toggle));
    static const char *const states[] = {"hide-empty", "wrap"};
    for (guint i = 0; i < 2; i++) {
        GSimpleAction *action = g_simple_action_new_stateful(states[i], NULL, g_variant_new_boolean(i == 1));
        g_signal_connect(action, "change-state", G_CALLBACK(toggle_state), in);
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
        g_object_unref(action);
    }
    gtk_widget_insert_action_group(in->paned, "inspector", G_ACTION_GROUP(actions));

    in->left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bar = pane_bar();
    GtkWidget *title = gtk_label_new("Event details");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(bar), title);
    gtk_box_append(GTK_BOX(bar), text_button("\xef\xbc\x8b", "Expand event details", "inspector.expand-details"));
    gtk_box_append(GTK_BOX(bar), text_button("\xe2\x88\x92", "Collapse event details", "inspector.collapse-details"));
    GtkWidget *close = close_button("Close event details");
    g_object_set_data(G_OBJECT(close), "pane", "details");
    g_signal_connect(close, "clicked", G_CALLBACK(close_pane), app);
    gtk_box_append(GTK_BOX(bar), close);
    gtk_box_append(GTK_BOX(in->left), bar);
    in->details = make_tree(&in->detail_store, "Field", "Value");
    gtk_box_append(GTK_BOX(in->left), scrolled(in->details));
    gtk_paned_set_start_child(GTK_PANED(in->paned), in->left);

    in->right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    bar = pane_bar();
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Source record"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    in->source_names = gtk_string_list_new(NULL);
    in->source = gtk_drop_down_new(G_LIST_MODEL(in->source_names), NULL);
    gtk_widget_set_size_request(in->source, 190, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(in->source), GTK_ACCESSIBLE_PROPERTY_LABEL, "Record data source", -1);
    gtk_box_append(GTK_BOX(bar), in->source);
    in->mode = gtk_drop_down_new_from_strings((const char *[]){"Field tree", "Formatted JSON", "Row / cell text", NULL});
    gtk_accessible_update_property(GTK_ACCESSIBLE(in->mode), GTK_ACCESSIBLE_PROPERTY_LABEL, "Record presentation", -1);
    gtk_box_append(GTK_BOX(bar), in->mode);
    GMenu *options = g_menu_new();
    g_menu_append(options, "Expand branches", "inspector.expand");
    g_menu_append(options, "Collapse branches", "inspector.collapse");
    g_menu_append(options, "Hide empty fields", "inspector.hide-empty");
    g_menu_append(options, "Wrap text", "inspector.wrap");
    g_menu_append(options, "Copy this data source", "inspector.copy");
    GtkWidget *more = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(more), "\xe2\x8b\xaf");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(options));
    gtk_widget_add_css_class(more, "tool");
    gtk_widget_set_tooltip_text(more, "Source record options");
    gtk_box_append(GTK_BOX(bar), more);
    close = close_button("Close source record");
    g_object_set_data(G_OBJECT(close), "pane", "source");
    g_signal_connect(close, "clicked", G_CALLBACK(close_pane), app);
    gtk_box_append(GTK_BOX(bar), close);
    gtk_box_append(GTK_BOX(in->right), bar);
    in->pages = gtk_stack_new();
    in->fields = make_tree(&in->field_store, "Field", "Value");
    gtk_stack_add_named(GTK_STACK(in->pages), scrolled(in->fields), "tree");
    in->text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(in->text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(in->text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(in->text), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(in->text, "mono");
    gtk_stack_add_named(GTK_STACK(in->pages), scrolled(in->text), "text");
    gtk_widget_set_vexpand(in->pages, TRUE);
    gtk_box_append(GTK_BOX(in->right), in->pages);
    GtkWidget *find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(find_row, 3);
    gtk_widget_set_margin_bottom(find_row, 3);
    gtk_widget_set_margin_start(find_row, 3);
    gtk_widget_set_margin_end(find_row, 3);
    in->find = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(in->find), "Find in this record\xe2\x80\xa6");
    gtk_accessible_update_property(GTK_ACCESSIBLE(in->find), GTK_ACCESSIBLE_PROPERTY_LABEL, "Find in this record", -1);
    gtk_widget_set_hexpand(in->find, TRUE);
    g_signal_connect(in->find, "activate", G_CALLBACK(find_activated), in);
    gtk_box_append(GTK_BOX(find_row), in->find);
    in->find_status = gtk_label_new("");
    gtk_box_append(GTK_BOX(find_row), in->find_status);
    GtkWidget *next = gtk_button_new_with_label("Next");
    gtk_widget_add_css_class(next, "tool");
    gtk_widget_set_tooltip_text(next, "Find next match");
    g_signal_connect(next, "clicked", G_CALLBACK(find_activated), in);
    gtk_box_append(GTK_BOX(find_row), next);
    gtk_box_append(GTK_BOX(in->right), find_row);
    gtk_paned_set_end_child(GTK_PANED(in->paned), in->right);
    gtk_paned_set_position(GTK_PANED(in->paned), 520);

    g_signal_connect(in->fields, "test-expand-row", G_CALLBACK(field_test_expand), in);
    g_signal_connect(in->fields, "row-expanded", G_CALLBACK(field_expanded), in);
    g_signal_connect(in->fields, "row-collapsed", G_CALLBACK(field_collapsed), in);
    g_signal_connect(in->details, "row-activated", G_CALLBACK(detail_activated), in);
    g_signal_connect(in->details, "cursor-changed", G_CALLBACK(detail_selected), in);
    g_signal_connect(in->source, "notify::selected", G_CALLBACK(selection_notify), in);
    g_signal_connect(in->mode, "notify::selected", G_CALLBACK(selection_notify), in);
    GtkWidget *trees[] = {in->details, in->fields};
    for (int i = 0; i < 2; i++) {
        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        g_signal_connect(click, "pressed", G_CALLBACK(tree_context), in);
        gtk_widget_add_controller(trees[i], GTK_EVENT_CONTROLLER(click));
        GtkEventController *keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(tree_key), in);
        gtk_widget_add_controller(trees[i], keys);
    }
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(text_pressed), in);
    gtk_widget_add_controller(in->text, GTK_EVENT_CONTROLLER(click));
    return in;
}

GtkWidget *inspector_widget(Inspector *in) {
    return in->paned;
}

GtkWidget *inspector_pane(Inspector *in, int index) {
    return index == 0 ? in->left : in->right;
}

void inspector_focus_find(Inspector *in) {
    gtk_widget_grab_focus(in->find);
    gtk_editable_select_region(GTK_EDITABLE(in->find), 0, -1);
}

void inspector_set_vertical(Inspector *in, gboolean vertical) {
    gtk_orientable_set_orientation(GTK_ORIENTABLE(in->paned), vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(in->paned), vertical ? 150 : 520);
}
