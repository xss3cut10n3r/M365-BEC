#include "../src/io/io.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
static char *root;

#define CHECK(condition)                                                                                      \
    do {                                                                                                      \
        checks++;                                                                                             \
        if (!(condition)) {                                                                                   \
            failures++;                                                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);                                        \
        }                                                                                                     \
    } while (0)

static char *file(const char *name) {
    return g_build_filename(root, name, NULL);
}

static char *put(const char *name, const char *text) {
    char *path = file(name);
    g_file_set_contents(path, text, -1, NULL);
    return path;
}

static sqlite3 *open_case(const char *name) {
    char *path = file(name);
    GError *error = NULL;
    sqlite3 *db = case_connect(path, &error);
    if (!db)
        printf("FAIL open %s: %s\n", name, error->message);
    g_free(path);
    return db;
}

static ImportState load(sqlite3 *db, const char *name, char **message) {
    char *path = file(name);
    GError *error = NULL;
    ImportState state = import_file(db, path, NULL, NULL, &error);
    if (message)
        *message = error ? g_strdup(error->message) : g_strdup("");
    g_clear_error(&error);
    g_free(path);
    return state;
}

static gint64 count(sqlite3 *db, const char *filter) {
    GError *error = NULL;
    gint64 value = count_rows(db, 0, filter, &error);
    if (error) {
        printf("FAIL filter %s: %s\n", filter, error->message);
        g_clear_error(&error);
    }
    return value;
}

static char *text_of(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = sql_prepare(db, sql, NULL);
    char *value = NULL;
    if (statement && sqlite3_step(statement) == SQLITE_ROW)
        value = g_strdup(column_text(statement, 0));
    sqlite3_finalize(statement);
    return value ? value : g_strdup("");
}

static gboolean save(sqlite3 *db, const char *name, const char *filter) {
    char *path = file(name);
    GError *error = NULL;
    gboolean ok = export_events(db, path, 0, filter, FALSE, "time", NULL, &error);
    if (error) {
        printf("export %s: %s\n", name, error->message);
        g_clear_error(&error);
    }
    g_free(path);
    return ok;
}

static void test_text(void) {
    char *stamp = utc_normalize("2026-01-02T11:04:00+05:30");
    CHECK(!strcmp(stamp, "2026-01-02T05:34:00.000000Z"));
    g_free(stamp);
    stamp = utc_normalize("not a date");
    CHECK(!*stamp);
    g_free(stamp);
    char *ip = ip_normalize("[2001:DB8:0:0:0:0:0:1]:443");
    CHECK(!strcmp(ip, "2001:db8::1"));
    g_free(ip);
    ip = ip_normalize("192.0.2.8:443");
    CHECK(!strcmp(ip, "192.0.2.8"));
    g_free(ip);
    GError *error = NULL;
    CHECK(query_new("ip = broken OR", &error) == NULL);
    g_clear_error(&error);
    Query *query = query_new("NOT (marked = false) AND field \"/a/b\" contains \"x\"", NULL);
    CHECK(query != NULL);
    query_free(query);
    GString *deep = g_string_new(NULL);
    for (int i = 0; i < 5000; i++)
        g_string_append_c(deep, '(');
    CHECK(query_new(deep->str, &error) == NULL);
    g_clear_error(&error);
    g_string_assign(deep, "");
    for (int i = 0; i < 200000; i++)
        g_string_append_c(deep, '[');
    CHECK(j_parse(deep->str, -1, 0, NULL) == NULL);
    g_string_free(deep, TRUE);
}

static void test_tabular(void) {
    sqlite3 *db = open_case("tabular.bec");
    char *source = put("InteractiveSignIns.csv",
                       "\xef\xbb\xbfRequest ID,Username,Incoming token type,Incoming token type,IP address,Date (UTC)\r\n"
                       "r1,user@example.org,\"first\nline\",second,[2001:db8::1]:443,2026-01-02T12:00:00Z\r\n"
                       "r2,=other@example.org,x,,203.0.113.7,2026-01-03T12:00:00Z\r\n");
    CHECK(load(db, "InteractiveSignIns.csv", NULL) == IMPORT_DONE);
    CHECK(load(db, "InteractiveSignIns.csv", NULL) == IMPORT_DUPLICATE);
    CHECK(count(db, "") == 2);
    CHECK(count(db, "ip = \"2001:db8::1\" AND mailbox = \"USER@example.org\"") == 1);
    CHECK(count(db, "field \"/@2:Incoming token type\" = \"second\"") == 1);
    CHECK(count(db, "field \"/@1:Incoming token type\" = \"second\"") == 0);
    CHECK(count(db, "source = \"interactive sign-ins\"") == 2);
    char *raw = text_of(db, "SELECT raw FROM events WHERE rownum=2");
    CHECK(strstr(raw, "\"first\\nline\",\"second\"") != NULL);
    g_free(raw);
    // Failed imports change nothing.
    g_free(put("bad.csv", "Operation,AuditData\nOK,{}\nBroken,\"unterminated"));
    char *message = NULL;
    CHECK(load(db, "bad.csv", &message) == IMPORT_FAILED);
    CHECK(*message);
    g_free(message);
    g_free(put("bad.xlsx", "this is not a workbook"));
    CHECK(load(db, "bad.xlsx", NULL) == IMPORT_FAILED);
    g_free(put("notes.txt", "x"));
    CHECK(load(db, "notes.txt", NULL) == IMPORT_FAILED);
    CHECK(sql_int(db, "SELECT count(*) FROM files", NULL) == 1);
    CHECK(sql_int(db, "SELECT count(*) FROM logs", NULL) == 1);
    g_free(put("audit.csv", "Operation,AuditData\nTest,{\n"));
    CHECK(load(db, "audit.csv", NULL) == IMPORT_DONE);
    CHECK(sql_int(db, "SELECT issues FROM logs WHERE name='audit.csv'", NULL) == 1);
    // Verdicts layer and undo.
    GPtrArray *pairs = g_ptr_array_new();
    g_ptr_array_add(pairs, "203.0.113.7");
    g_ptr_array_add(pairs, "");
    CHECK(assign_verdict(db, "ip", pairs, "Bad", NULL));
    CHECK(count(db, "verdict = Bad") == 1);
    CHECK(count(db, "marked = true") == 1);
    CHECK(save(db, "out.csv", ""));
    CHECK(save(db, "out.xlsx", ""));
    char *exported = NULL, *path = file("out.csv");
    g_file_get_contents(path, &exported, NULL, NULL);
    CHECK(exported && strstr(exported, ",'=other@example.org,"));
    CHECK(exported && strstr(exported, "Bad \xc2\xb7 IP"));
    g_free(exported);
    g_free(path);
    CHECK(load(db, "out.xlsx", NULL) == IMPORT_DONE);
    CHECK(count(db, "file = \"out.xlsx\" AND source = \"Interactive sign-ins\"") == 2);
    CHECK(count(db, "file = \"out.xlsx\" AND actor = \"=other@example.org\"") == 1);
    CHECK(undo_verdict(db, NULL));
    CHECK(count(db, "marked = true") == 0);
    g_ptr_array_free(pairs, TRUE);
    // Evidence survives byte for byte.
    char *copy = file("copy.csv"), *original = NULL, *recovered = NULL;
    gsize a = 0, b = 0;
    CHECK(extract_evidence(db, 1, copy, NULL));
    g_file_get_contents(source, &original, &a, NULL);
    g_file_get_contents(copy, &recovered, &b, NULL);
    CHECK(a == b && original && recovered && !memcmp(original, recovered, a));
    CHECK(!extract_evidence(db, 1, source, NULL));
    CHECK(!save(db, "InteractiveSignIns.csv", ""));
    CHECK(!sql_run(db, "UPDATE chunks SET data=x'00'", NULL));
    CHECK(!sql_run(db, "DELETE FROM files", NULL));
    CHECK(!sql_run(db, "UPDATE events SET raw='[]'", NULL));
    g_free(original);
    g_free(recovered);
    g_free(copy);
    g_free(source);
    sqlite3_close(db);
}

static void test_json(void) {
    sqlite3 *db = open_case("json.bec");
    g_free(put("graph.json",
               "{\"@odata.context\":\"x\",\"@odata.nextLink\":\"more\",\"value\":["
               "{\"id\":\"g-0\",\"createdDateTime\":\"2026-01-05T06:00:00Z\",\"userPrincipalName\":\"a@example.org\","
               "\"ipAddress\":\"203.0.113.7\",\"status\":{\"errorCode\":50126,\"failureReason\":\"Bad password\"},"
               "\"big\":12345678901234567890,\"ratio\":2.50,\"exp\":1e3,\"flag\":true,\"nothing\":null,"
               "\"policies\":[{\"name\":\"MFA\",\"result\":\"success\"},{\"name\":\"Legacy\",\"result\":\"notApplied\"}]},"
               "{\"id\":\"g-1\",\"createdDateTime\":\"2026-01-05T05:00:00Z\",\"userPrincipalName\":\"b@example.org\","
               "\"ipAddress\":\"192.0.2.44\",\"status\":{\"errorCode\":0}}]}"));
    g_free(put("array.json",
               "[{\"CreationTime\":\"2026-01-05T09:00:00\",\"Operation\":\"Send\",\"UserId\":\"cfo@example.org\","
               "\"ClientIP\":\"198.51.100.23:443\",\"RecordType\":2,\"Item\":{\"Subject\":\"Zahlung\"}},"
               "{\"Operation\":\"HardDelete\",\"CreationTime\":\"2026-01-05T09:05:00\",\"Extra\":{\"deep\":[{\"k\":\"v\"}]}}]"));
    g_free(put("single.json", "\xef\xbb\xbf{\"Operation\":\"New-InboxRule\",\"CreationTime\":\"2026-01-04T01:00:00\",\"UserId\":\"x@example.org\"}"));
    g_free(put("empty.json", " [ ] "));
    g_free(put("broken.json", "[{\"Operation\": \"Send\"}, {\"Operation\": "));
    g_free(put("ambiguous.json", "{\"value\": [{\"a\": 1}], \"Operation\": \"Send\"}"));
    g_free(put("scalar.json", "[1, 2]"));
    g_free(put("duplicate.json", "[{\"a\": 1, \"a\": 2}]"));
    g_free(put("events.csv", "Date (UTC),Username,Operation,IP address\n2026-01-05T07:00:00Z,c@example.org,Read,203.0.113.7\n"));
    CHECK(load(db, "graph.json", NULL) == IMPORT_DONE);
    CHECK(load(db, "array.json", NULL) == IMPORT_DONE);
    CHECK(load(db, "single.json", NULL) == IMPORT_DONE);
    CHECK(load(db, "empty.json", NULL) == IMPORT_DONE);
    CHECK(load(db, "events.csv", NULL) == IMPORT_DONE);
    gint64 files = sql_int(db, "SELECT count(*) FROM files", NULL);
    static const char *const rejected[] = {"broken.json", "ambiguous.json", "scalar.json", "duplicate.json"};
    for (guint i = 0; i < G_N_ELEMENTS(rejected); i++) {
        char *message = NULL;
        CHECK(load(db, rejected[i], &message) == IMPORT_FAILED);
        CHECK(strlen(message) > 10);
        g_free(message);
    }
    CHECK(sql_int(db, "SELECT count(*) FROM files", NULL) == files);
    CHECK(sql_int(db, "SELECT count(*) FROM events", NULL) == 6);
    CHECK(sql_int(db, "SELECT total FROM logs WHERE name='empty.json'", NULL) == 0);
    CHECK(sql_int(db, "SELECT count(*) FROM logs WHERE name='graph.json' AND warning LIKE '%nextLink%'", NULL) == 1);
    CHECK(load(db, "graph.json", NULL) == IMPORT_DUPLICATE);
    // Nested fields filter like columns.
    CHECK(count(db, "field \"/status/errorCode\" = \"50126\"") == 1);
    CHECK(count(db, "field \"/policies/*/result\" = \"notApplied\"") == 1);
    CHECK(count(db, "field \"/Extra/deep/0/k\" = \"v\"") == 1);
    CHECK(count(db, "exists \"/Item\"") == 1);
    CHECK(count(db, "exists \"/Extra\"") == 1);
    CHECK(count(db, "exists \"/nothing\"") == 1);
    CHECK(count(db, "field \"/flag\" = \"true\"") == 1);
    CHECK(count(db, "subject = \"Zahlung\"") == 1);
    CHECK(count(db, "result = \"Bad password\"") == 1);
    CHECK(count(db, "result = \"Success\"") == 1);
    CHECK(count(db, "ip = 198.51.100.23 AND actor = \"cfo@example.org\"") == 1);
    CHECK(count(db, "source = \"Graph sign-ins\"") == 2);
    CHECK(count(db, "time >= 2026-01-05T06:00:00Z AND time < 2026-01-05T06:00:01Z") == 1);
    char *sheet = text_of(db, "SELECT sheet||'/'||(rownum-1) FROM events e JOIN logs l ON l.id=e.log WHERE actor='b@example.org'");
    CHECK(!strcmp(sheet, "/value/1"));
    g_free(sheet);
    // Mark, then export mixed sources.
    GPtrArray *pairs = g_ptr_array_new();
    g_ptr_array_add(pairs, "203.0.113.7");
    g_ptr_array_add(pairs, "");
    CHECK(assign_verdict(db, "ip", pairs, "Bad", NULL));
    g_ptr_array_free(pairs, TRUE);
    CHECK(save(db, "all.json", ""));
    CHECK(save(db, "nested.json", "field \"/status/errorCode\" = \"50126\" OR operation = \"Read\""));
    char *text = NULL, *path = file("all.json");
    g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    JDoc *doc = text ? j_parse(text, -1, J_STRICT_KEYS, NULL) : NULL;
    CHECK(doc && doc->root->t == J_ARR && doc->root->n == 6);
    if (doc && doc->root->n == 6) {
        const char *previous = "";
        for (guint i = 0; i < doc->root->n; i++) {
            const JV *stamp = j_get(doc->root->u.a[i], "Time (UTC)");
            CHECK(stamp && strcmp(previous, stamp->u.s) <= 0);
            previous = stamp ? stamp->u.s : previous;
            CHECK(j_get(doc->root->u.a[i], "ID")->t == J_NUM);
            CHECK(strlen(j_get(doc->root->u.a[i], "SHA-256")->u.s) == 64);
        }
    }
    CHECK(text && strstr(text, "\"Location\":\"/value/0\""));
    CHECK(text && strstr(text, "\"Location\":\"row 2\""));
    CHECK(text && strstr(text, "\"Location\":\"#\""));
    CHECK(text && strstr(text, "\"big\":12345678901234567890,\"ratio\":2.50,\"exp\":1e3,\"flag\":true,\"nothing\":null"));
    CHECK(text && strstr(text, "\"Record\":{\"Operation\":\"HardDelete\",\"CreationTime\":\"2026-01-05T09:05:00\",\"Extra\""));
    CHECK(text && strstr(text, "\"Record\":{\"Date (UTC)\":\"2026-01-05T07:00:00Z\""));
    j_free(doc);
    g_free(text);
    // The analyzer accepts its exports.
    sqlite3 *fresh = open_case("fresh.bec");
    CHECK(load(fresh, "nested.json", NULL) == IMPORT_DONE);
    CHECK(count(fresh, "") == 2);
    CHECK(count(fresh, "verdict = Bad") == 2);
    CHECK(count(fresh, "source = \"Graph sign-ins\"") == 1);
    CHECK(count(fresh, "source = \"Tabular log\"") == 1);
    CHECK(count(fresh, "field \"/Record/status/errorCode\" = \"50126\"") == 1);
    CHECK(count(fresh, "field \"/Record/big\" = \"12345678901234567890\"") == 1);
    CHECK(count(fresh, "field \"/Location\" = \"/value/0\" AND field \"/File\" = \"graph.json\"") == 1);
    CHECK(count(fresh, "ip = 203.0.113.7") == 2);
    char *first = text_of(fresh, "SELECT e.actor FROM events e ORDER BY (e.time=''),e.time,e.id LIMIT 1");
    CHECK(!strcmp(first, "a@example.org"));
    g_free(first);
    CHECK(save(fresh, "again.json", ""));
    text = NULL;
    path = file("again.json");
    g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    CHECK(text && strstr(text, "\"ratio\":2.50,\"exp\":1e3,\"flag\":true,\"nothing\":null"));
    CHECK(text && strstr(text, "\"Verdict\":\"Bad\""));
    g_free(text);
    CHECK(load(fresh, "all.json", NULL) == IMPORT_DONE);
    CHECK(sql_int(fresh, "SELECT total FROM logs WHERE name='all.json'", NULL) == 6);
    sqlite3_close(fresh);
    sqlite3_close(db);
}

static void test_statistics(void) {
    sqlite3 *db = open_case("json.bec");
    for (int i = 0; i < REPORT_COUNT; i++) {
        GError *error = NULL;
        GPtrArray *rows = stats_calculate(db, REPORTS[i].key, 0, "", 3600, &error);
        CHECK(rows != NULL);
        for (guint k = 0; rows && k < rows->len; k++) {
            StatRow *row = rows->pdata[k];
            char *drill = stats_drilldown(REPORTS[i].key, row, 3600);
            CHECK(count(db, drill) == row->count);
            g_free(drill);
        }
        if (rows)
            g_ptr_array_free(rows, TRUE);
        g_clear_error(&error);
    }
    sqlite3_close(db);
}

int main(void) {
    root = g_dir_make_tmp("bec-test-XXXXXX", NULL);
    test_text();
    test_tabular();
    test_json();
    test_statistics();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
