#include "../src/io/io.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <xlsxwriter.h>
#include <zip.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    gboolean ok = export_events(db, path, 0, filter, FALSE, "time", NULL, NULL, &error);
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
    // Imported verdicts undo first.
    CHECK(undo_verdict(db, NULL) && count(db, "marked = true") == 2);
    CHECK(undo_verdict(db, NULL));
    CHECK(count(db, "marked = true") == 0);
    g_ptr_array_free(pairs, TRUE);
    // Empty paths match nothing.
    CHECK(count(db, "exists \"/\"") == 0 && count(db, "field \"/\" = \"x\"") == 0);
    CHECK(count(db, "field \"//\" contains \"x\"") == 0 && count(db, "exists \"/~\"") == 0);
    // Filters mark matching events.
    GError *failed = NULL;
    gint64 every = count(db, "");
    gint64 hits = count(db, "ip = 203.0.113.7");
    CHECK(hits > 0 && mark_matches(db, 0, "ip = 203.0.113.7", "Unsure", 0, NULL) == hits);
    CHECK(count(db, "verdict = Unsure AND markreason = Event") == hits);
    CHECK(mark_matches(db, 0, "", "Good", 1, &failed) == -1 && failed != NULL);
    g_clear_error(&failed);
    CHECK(mark_matches(db, 0, "madeup = 1", "Good", 0, &failed) == -1);
    CHECK(failed && strstr(failed->message, "Fields: ") && strstr(failed->message, "mailbox"));
    g_clear_error(&failed);
    CHECK(mark_matches(db, 0, "ip = 198.51.100.250", "Bad", 0, NULL) == 0);
    CHECK(mark_matches(db, 0, "marked = true", "Bad", 0, NULL) == hits);
    CHECK(count(db, "verdict = Bad") == hits && count(db, "verdict = Unsure") == 0);
    CHECK(save(db, "tinted.xlsx", "marked = true"));
    CHECK(undo_verdict(db, NULL) && undo_verdict(db, NULL));
    CHECK(count(db, "marked = true") == 0 && count(db, "") == every);
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

// Padded, undated and nested values.
static void test_consistency(void) {
    sqlite3 *db = open_case("odd.bec");
    g_free(put("padded.csv", "Date (UTC),Username,IP address,Status,Location,Application\n"
                             "2026-01-01T12:00:00Z,a@example.org,192.0.2.8,Success,\" Leeds, GB \",App\n"
                             "2026-01-01T13:00:00Z,b@example.org,192.0.2.9,Success,\"Leeds, GB\",App \n"
                             "2026-01-01T14:00:00Z, c@example.org ,192.0.2.9, Success ,\"LEEDS, gb\",\n"
                             ",d@example.org,192.0.2.9,Failure,,\n"));
    g_free(put("graph.json", "{\"value\":[{\"createdDateTime\":\"2026-01-02T00:00:00Z\",\"userPrincipalName\":\"e@example.org\","
                             "\"location\":{\"city\":\"X\",\"countryOrRegion\":\"US\"},\"status\":{\"errorCode\":0},"
                             "\"appDisplayName\":\"Portal\",\"clientAppUsed\":\"Browser\",\"conditionalAccessStatus\":\"success\","
                             "\"deviceDetail\":{\"deviceId\":\"dev-9\",\"operatingSystem\":\"Windows10\"}}]}"));
    CHECK(load(db, "padded.csv", NULL) == IMPORT_DONE && load(db, "graph.json", NULL) == IMPORT_DONE);
    gint64 every = count(db, ""), dated = count(db, "time >= \"0001-01-01\"");
    CHECK(every == 5 && dated == 4);
    CHECK(count(db, "time < \"2026-01-01T13:00:00Z\"") == 1 && count(db, "time <= \"2026-01-01T13:00:00Z\"") == 2);
    CHECK(count(db, "time < \"2030-01-01\"") + count(db, "time >= \"2030-01-01\"") == dated);
    CHECK(count(db, "file = padded.csv AND result = \"success\"") == 3 && count(db, "file = padded.csv AND result = \" SUCCESS \"") == 3);
    CHECK(count(db, "file = padded.csv AND result != \"Success\"") == 1 && count(db, "actor = \"c@example.org\"") == 1);
    CHECK(count(db, "location = \"leeds, gb\"") == 3 && count(db, "field \"/Location\" = \"Leeds, GB\"") == 3);
    CHECK(count(db, "location contains \" leeds\"") == 1 && count(db, "application = \"app\"") == 2);
    CHECK(count(db, "exists \"/\"") == 0 && count(db, "field \"/\" = \"x\"") == 0 && count(db, "exists \"//\"") == 0);
    // Graph sign-ins fill named fields.
    CHECK(count(db, "location = \"X, US\"") == 1 && count(db, "device = dev-9") == 1 && count(db, "os = windows10") == 1);
    CHECK(count(db, "application = Portal") == 1 && count(db, "client = Browser") == 1);
    CHECK(count(db, "conditionalaccess = success") == 1 && count(db, "location contains \"us\"") == 1);
    for (int i = 0; i < REPORT_COUNT; i++) {
        GPtrArray *rows = stats_calculate(db, REPORTS[i].key, 0, "", 3600, NULL);
        CHECK(rows != NULL);
        for (guint k = 0; rows && k < rows->len; k++) {
            StatRow *row = rows->pdata[k];
            char *drill = stats_drilldown(REPORTS[i].key, row, 3600);
            if (count(db, drill) != row->count)
                printf("drill %s: %s\n", REPORTS[i].key, drill);
            CHECK(count(db, drill) == row->count);
            g_free(drill);
        }
        if (rows)
            g_ptr_array_free(rows, TRUE);
    }
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

static char *slurp(const char *path) {
    char *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    return text ? text : g_strdup("");
}

static gboolean export_to(sqlite3 *db, const char *path, const char *filter) {
    return export_events(db, path, 0, filter, FALSE, "time", NULL, NULL, NULL);
}

// Aliases reach the same file.
static void test_aliases(void) {
    sqlite3 *db = open_case("alias.bec");
    char *real = file("real"), *alias = file("alias"), *hard = file("hard.csv");
    g_mkdir(real, 0700);
    char *source = g_build_filename(real, "ev.csv", NULL), *via = g_build_filename(alias, "ev.csv", NULL);
    char *other = g_build_filename(alias, "other.csv", NULL);
    g_file_set_contents(source, "Operation\nOne\n", -1, NULL);
#ifdef G_OS_WIN32
    char *command = g_strdup_printf("cmd /c mklink /J \"%s\" \"%s\"", alias, real);
    g_spawn_command_line_sync(command, NULL, NULL, NULL, NULL);
    g_free(command);
    CreateHardLinkA(hard, source, NULL);
#else
    CHECK(symlink(real, alias) == 0);
    CHECK(link(source, hard) == 0);
#endif
    CHECK(g_file_test(via, G_FILE_TEST_EXISTS) && g_file_test(hard, G_FILE_TEST_EXISTS));
    CHECK(import_file(db, source, NULL, NULL, NULL) == IMPORT_DONE);
    CHECK(!export_to(db, source, "") && !export_to(db, via, "") && !export_to(db, hard, ""));
    CHECK(!extract_evidence(db, 1, via, NULL));
    CHECK(export_to(db, other, ""));
    char *kept = slurp(source);
    CHECK(!strcmp(kept, "Operation\nOne\n"));
    // Deleted evidence keeps its name.
    g_remove(hard);
    g_remove(source);
    CHECK(!export_to(db, via, ""));
    g_free(kept);
    g_free(real);
    g_free(alias);
    g_free(hard);
    g_free(source);
    g_free(via);
    g_free(other);
    sqlite3_close(db);
}

// Counts come from the snapshot.
static void test_export_count(void) {
    sqlite3 *db = open_case("count.bec"), *other = open_case("count.bec");
    g_free(put("three.csv", "Operation\nA\nB\nC\n"));
    g_free(put("late.csv", "Operation\nLate\n"));
    CHECK(load(db, "three.csv", NULL) == IMPORT_DONE);
    char *path = file("counted.json");
    gint64 written = -1;
    CHECK(export_events(db, path, 0, "operation != B", FALSE, "time", NULL, &written, NULL) && written == 2);
    CHECK(load(other, "late.csv", NULL) == IMPORT_DONE);
    CHECK(export_events(db, path, 0, "", FALSE, "time", NULL, &written, NULL) && written == 4);
    char *text = slurp(path);
    JDoc *doc = j_parse(text, -1, 0, NULL);
    CHECK(doc && doc->root->n == 4);
    j_free(doc);
    g_free(text);
    g_free(path);
    sqlite3_close(other);
    sqlite3_close(db);
}

// Marked exports keep their scope.
static void test_mark_scope(void) {
    sqlite3 *db = open_case("scope.bec");
    g_free(put("scope.csv", "IP,Operation\n192.0.2.1,First\n192.0.2.2,Second\n192.0.2.3,Third\n"));
    CHECK(load(db, "scope.csv", NULL) == IMPORT_DONE);
    GPtrArray *pairs = g_ptr_array_new();
    g_ptr_array_add(pairs, "192.0.2.1");
    g_ptr_array_add(pairs, "");
    CHECK(assign_verdict(db, "ip", pairs, "Good", NULL));
    pairs->pdata[0] = "192.0.2.2";
    CHECK(assign_verdict(db, "ip", pairs, "Bad", NULL));
    char *scope = g_strdup_printf("markbatch = %" G_GINT64_FORMAT, last_batch(db));
    CHECK(count(db, scope) == 1 && count(db, "marked = true") == 2);
    g_free(scope);
    CHECK(assign_verdict(db, "ip", pairs, "Unmarked", NULL));
    scope = g_strdup_printf("(markbatch = %" G_GINT64_FORMAT ") AND operation = Second AND verdict = Unmarked", last_batch(db));
    CHECK(count(db, scope) == 1);
    g_free(scope);
    // The filter stops matching afterwards.
    CHECK(mark_matches(db, 0, "verdict = Unmarked AND operation = Third", "Bad", 0, NULL) == 1);
    scope = g_strdup_printf("markbatch = %" G_GINT64_FORMAT, last_batch(db));
    CHECK(count(db, scope) == 1 && count(db, "verdict = Unmarked AND operation = Third") == 0);
    char *both = g_strconcat(scope, " AND operation = Third AND verdict = Bad", NULL);
    CHECK(count(db, both) == 1 && count(db, "markbatch = 0") == 0);
    g_free(both);
    g_free(scope);
    g_ptr_array_free(pairs, TRUE);
    sqlite3_close(db);
}

static gpointer first_import(gpointer name) {
    sqlite3 *db = open_case("race.bec");
    gboolean ok = db && load(db, name, NULL) == IMPORT_DONE;
    sqlite3_close(db);
    return GINT_TO_POINTER(ok);
}

// Two importers create one case.
static void test_first_imports(void) {
    char *path = file("race.bec");
    g_free(put("race-a.csv", "Operation\nA\n"));
    g_free(put("race-b.csv", "Operation\nB\n"));
    sqlite3 *lock = NULL;
    CHECK(sqlite3_open(path, &lock) == SQLITE_OK && sql_run(lock, "BEGIN IMMEDIATE", NULL));
    GThread *a = g_thread_new("a", first_import, "race-a.csv"), *b = g_thread_new("b", first_import, "race-b.csv");
    g_usleep(500000);
    CHECK(sql_run(lock, "ROLLBACK", NULL));
    sqlite3_close(lock);
    CHECK(GPOINTER_TO_INT(g_thread_join(a)));
    CHECK(GPOINTER_TO_INT(g_thread_join(b)));
    sqlite3 *db = open_case("race.bec");
    CHECK(count(db, "") == 2 && sql_int(db, "SELECT count(*) FROM files", NULL) == 2);
    sqlite3_close(db);
    g_free(path);
}

// New cases are private.
static void test_permissions(void) {
#ifdef G_OS_UNIX
    mode_t before = umask(022);
    sqlite3 *db = open_case("private.bec");
    g_free(put("private.csv", "Operation\nA\n"));
    CHECK(load(db, "private.csv", NULL) == IMPORT_DONE);
    static const char *const names[] = {"private.bec", "private.bec-wal", "private.bec-shm"};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        char *path = file(names[i]);
        GStatBuf info;
        CHECK(g_stat(path, &info) == 0 && (info.st_mode & 077) == 0);
        g_free(path);
    }
    sqlite3_close(db);
    // Chosen access is kept.
    char *path = file("private.bec");
    CHECK(g_chmod(path, 0640) == 0);
    db = open_case("private.bec");
    GStatBuf info;
    CHECK(g_stat(path, &info) == 0 && (info.st_mode & 0777) == 0640);
    sqlite3_close(db);
    g_free(path);
    umask(before);
#endif
}

// Settled cases open in place.
static void test_read_only(void) {
#ifdef G_OS_UNIX
    if (geteuid() == 0)
        return;
    char *folder = file("locked"), *path = g_build_filename(folder, "ro.bec", NULL);
    char *source = put("ro.csv", "Operation\nOne\n");
    g_mkdir(folder, 0700);
    GError *error = NULL;
    sqlite3 *db = case_connect(path, NULL);
    CHECK(db && import_file(db, source, NULL, NULL, NULL) == IMPORT_DONE);
    sqlite3_close(db);
    CHECK(g_chmod(path, 0444) == 0 && g_chmod(folder, 0555) == 0);
    db = case_connect(path, &error);
    CHECK(db != NULL);
    if (db) {
        CHECK(count(db, "operation = One") == 1);
        CHECK(mark_matches(db, 0, "operation = One", "Bad", 0, NULL) == -1);
        GPtrArray *rows = stats_calculate(db, "operations", 0, "", 3600, NULL);
        CHECK(rows && rows->len == 1);
        if (rows)
            g_ptr_array_free(rows, TRUE);
        sqlite3_close(db);
    }
    g_clear_error(&error);
    db = case_read(path, NULL);
    CHECK(db && count(db, "") == 1);
    sqlite3_close(db);
    GDir *listing = g_dir_open(folder, 0, NULL);
    g_dir_read_name(listing);
    CHECK(g_dir_read_name(listing) == NULL);
    g_dir_close(listing);
    // Live WAL data is never skipped.
    CHECK(g_chmod(folder, 0755) == 0 && g_chmod(path, 0644) == 0);
    sqlite3 *writer = case_connect(path, NULL);
    g_free(put("ro-more.csv", "Operation\nTwo\n"));
    CHECK(writer && load(writer, "ro-more.csv", NULL) == IMPORT_DONE);
    CHECK(g_chmod(path, 0444) == 0 && g_chmod(folder, 0555) == 0);
    db = case_connect(path, NULL);
    CHECK(!db || count(db, "") == 2);
    sqlite3_close(db);
    CHECK(g_chmod(folder, 0755) == 0 && g_chmod(path, 0644) == 0);
    sqlite3_close(writer);
    g_free(source);
    g_free(folder);
    g_free(path);
#endif
}

// Idle cases copy as one file.
static void test_copy(void) {
    sqlite3 *reader = open_case("live.bec"), *writer = open_case("live.bec");
    g_free(put("live-a.csv", "Operation\nOne\n"));
    g_free(put("live-b.csv", "Operation\nTwo\n"));
    CHECK(load(reader, "live-a.csv", NULL) == IMPORT_DONE && count(reader, "") == 1);
    CHECK(load(writer, "live-b.csv", NULL) == IMPORT_DONE);
    CHECK(mark_matches(writer, 0, "operation = Two", "Bad", 0, NULL) == 1);
    char *path = file("live.bec"), *copy = file("handoff.bec"), *bytes = NULL;
    gsize size = 0;
    CHECK(g_file_get_contents(path, &bytes, &size, NULL) && g_file_set_contents(copy, bytes, (gssize)size, NULL));
    sqlite3 *copied = open_case("handoff.bec");
    CHECK(count(copied, "") == 2 && count(copied, "verdict = Bad AND operation = Two") == 1);
    CHECK(undo_verdict(writer, NULL));
    g_free(bytes);
    CHECK(g_file_get_contents(path, &bytes, &size, NULL));
    sqlite3_close(copied);
    CHECK(g_file_set_contents(copy, bytes, (gssize)size, NULL));
    copied = open_case("handoff.bec");
    CHECK(count(copied, "") == 2 && count(copied, "marked = true") == 0);
    sqlite3_close(copied);
    sqlite3_close(writer);
    sqlite3_close(reader);
    g_free(bytes);
    g_free(path);
    g_free(copy);
}

// Rewrites one workbook part.
static void zip_swap(const char *path, const char *entry, const char *from, const char *to) {
    zip_t *archive = zip_open(path, 0, NULL);
    zip_stat_t info;
    GString *text = g_string_new(NULL);
    if (archive && from && zip_stat(archive, entry, 0, &info) == 0) {
        zip_file_t *part = zip_fopen(archive, entry, 0);
        g_string_set_size(text, info.size);
        zip_fread(part, text->str, info.size);
        zip_fclose(part);
        g_string_replace(text, from, to, 0);
    } else {
        g_string_assign(text, to);
    }
    gboolean ok = archive && zip_file_add(archive, entry, zip_source_buffer(archive, text->str, text->len, 0), ZIP_FL_OVERWRITE) >= 0;
    CHECK(ok && zip_close(archive) == 0);
    g_string_free(text, TRUE);
}

static void write_cells(lxw_worksheet *sheet, int row, const char *first, const char *second) {
    worksheet_write_string(sheet, row, 0, first, NULL);
    if (second)
        worksheet_write_string(sheet, row, 1, second, NULL);
}

// Gaps, escapes and relocated sheets.
static void test_workbooks(void) {
    sqlite3 *db = open_case("books.bec");
    char *path = file("gaps.xlsx");
    lxw_workbook *book = workbook_new(path);
    lxw_format *bold = workbook_add_format(book);
    format_set_bold(bold);
    write_cells(workbook_add_worksheet(book, "Cover"), 0, "Status", NULL);
    lxw_worksheet *sheet = workbook_add_worksheet(book, "Evidence");
    write_cells(sheet, 1, "Operation", "Actor");
    write_cells(sheet, 2, "Send", "a");
    worksheet_write_blank(sheet, 3, 0, bold);
    write_cells(sheet, 5, "Delete", "b");
    write_cells(sheet, 6, "Short", NULL);
    CHECK(workbook_close(book) == LXW_NO_ERROR);
    CHECK(load(db, "gaps.xlsx", NULL) == IMPORT_DONE);
    CHECK(count(db, "sheet = Evidence") == 3 && count(db, "sheet = Cover") == 0);
    CHECK(count(db, "operation = Send AND row = 3 AND actor = a") == 1 && count(db, "operation = Delete AND row = 6") == 1);
    CHECK(count(db, "operation = Short AND row = 7 AND issue contains \"Expected 2 fields\"") == 1);
    CHECK(sql_int(db, "SELECT issues FROM logs WHERE sheet='Evidence'", NULL) == 1);
    g_free(path);
    // Only real escapes are decoded.
    path = file("escapes.xlsx");
    book = workbook_new(path);
    sheet = workbook_add_worksheet(book, "Shared");
    write_cells(sheet, 0, "Operation", "Subject");
    write_cells(sheet, 1, "Send", "one");
    write_cells(sheet, 2, "Send", "two");
    write_cells(workbook_add_worksheet(book, "Inline"), 0, "x", NULL);
    CHECK(workbook_close(book) == LXW_NO_ERROR);
    zip_swap(path, "xl/sharedStrings.xml", NULL,
             "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><si><t>Operation</t></si>"
             "<si><t>Subject</t></si><si><t>Send</t></si><si><t>box005F_item</t></si>"
             "<si><t>a_x005F_x000D_b_x000D_c_x00ZZ_</t></si></sst>");
    zip_swap(path, "xl/worksheets/sheet2.xml", NULL,
             "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
             "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Operation</t></is></c>"
             "<c r=\"B1\" t=\"inlineStr\"><is><t>Subject</t></is></c></row>"
             "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>Read</t></is></c>"
             "<c r=\"B2\" t=\"inlineStr\"><is><t>inx005F_line_x005F_</t></is></c></row></sheetData></worksheet>");
    CHECK(load(db, "escapes.xlsx", NULL) == IMPORT_DONE);
    CHECK(count(db, "sheet = Shared AND subject = \"box005F_item\"") == 1);
    CHECK(count(db, "sheet = Shared AND subject = \"a_x000D_b\\rc_x00ZZ_\"") == 1);
    CHECK(count(db, "sheet = Inline AND subject = \"inx005F_line_\"") == 1);
    g_free(path);
    // Relationships locate worksheets.
    path = file("moved.xlsx");
    book = workbook_new(path);
    write_cells(workbook_add_worksheet(book, "One"), 0, "Operation", NULL);
    sheet = workbook_add_worksheet(book, "Two");
    write_cells(sheet, 0, "Operation", NULL);
    write_cells(sheet, 1, "Second", NULL);
    CHECK(workbook_close(book) == LXW_NO_ERROR);
    zip_t *archive = zip_open(path, 0, NULL);
    CHECK(archive && zip_file_rename(archive, (zip_uint64_t)zip_name_locate(archive, "xl/worksheets/sheet2.xml", 0), "xl/logs/data.xml", 0) == 0);
    CHECK(archive && zip_close(archive) == 0);
    zip_swap(path, "xl/_rels/workbook.xml.rels", "worksheets/sheet2.xml", "logs/data.xml");
    zip_swap(path, "[Content_Types].xml", "/xl/worksheets/sheet2.xml", "/xl/logs/data.xml");
    CHECK(load(db, "moved.xlsx", NULL) == IMPORT_DONE);
    CHECK(count(db, "file = moved.xlsx AND sheet = Two AND operation = Second") == 1);
    g_free(path);
    sqlite3_close(db);
}

// Timeline exports reimport faithfully.
static void test_handoff(void) {
    sqlite3 *db = open_case("handoff-source.bec");
    g_free(put("verdicts.csv", "Operation,IP\nA,192.0.2.1\nB,192.0.2.2\nC,192.0.2.3\nD,192.0.2.4\n"));
    CHECK(load(db, "verdicts.csv", NULL) == IMPORT_DONE);
    CHECK(mark_matches(db, 0, "operation = A", "Good", 0, NULL) == 1 && mark_matches(db, 0, "operation = B", "Bad", 0, NULL) == 1);
    CHECK(mark_matches(db, 0, "operation = C", "Unsure", 0, NULL) == 1);
    static const char *const formats[] = {"csv", "xlsx", "json"};
    for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
        char *name = g_strconcat("handoff.", formats[i], NULL), *label = g_strconcat("handoff-", formats[i], ".bec", NULL);
        CHECK(save(db, name, ""));
        sqlite3 *fresh = open_case(label);
        CHECK(load(fresh, name, NULL) == IMPORT_DONE);
        CHECK(count(fresh, "verdict = Good AND operation = A") == 1 && count(fresh, "verdict = Bad AND operation = B") == 1);
        CHECK(count(fresh, "verdict = Unsure AND operation = C") == 1 && count(fresh, "verdict = Unmarked AND operation = D") == 1);
        sqlite3_close(fresh);
        g_free(name);
        g_free(label);
    }
    sqlite3_close(db);
    // Every listed address stays searchable.
    db = open_case("pair-source.bec");
    g_free(put("pair.csv", "Request ID,IP address,IP address (seen by resource)\nr1,192.0.2.11,192.0.2.12\n"));
    CHECK(load(db, "pair.csv", NULL) == IMPORT_DONE);
    for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
        char *name = g_strconcat("pair-out.", formats[i], NULL), *label = g_strconcat("pair-", formats[i], ".bec", NULL);
        CHECK(save(db, name, ""));
        sqlite3 *fresh = open_case(label);
        CHECK(load(fresh, name, NULL) == IMPORT_DONE);
        CHECK(count(fresh, "ip = 192.0.2.11") == 1 && count(fresh, "ip = 192.0.2.12") == 1 && count(fresh, "") == 1);
        sqlite3_close(fresh);
        g_free(name);
        g_free(label);
    }
    sqlite3_close(db);
}

// Group counts equal their drilldowns.
static gint64 report_total(sqlite3 *db, const char *key, const char *label) {
    GPtrArray *rows = stats_calculate(db, key, 0, "", 3600, NULL);
    gint64 total = rows ? 0 : -1;
    for (guint i = 0; rows && i < rows->len; i++) {
        StatRow *row = rows->pdata[i];
        char *drill = stats_drilldown(key, row, 3600);
        if (count(db, drill) != row->count) {
            printf("drill %s: %s\n", key, drill);
            total = -1000000;
        }
        if (!label || !strcmp(row->values[0], label))
            total += row->count;
        g_free(drill);
    }
    if (rows)
        g_ptr_array_free(rows, TRUE);
    return total;
}

// Provider fields feed shared pivots.
static void test_pivots(void) {
    sqlite3 *db = open_case("pivots.bec");
    g_free(put("trace.csv", "Received,SenderAddress,RecipientAddress,FromIP,ToIP,Status\n"
                            "2026-06-01T12:00:00Z,s@example.org,r@example.org,192.0.2.8,192.0.2.9,Delivered\n"));
    CHECK(load(db, "trace.csv", NULL) == IMPORT_DONE);
    CHECK(count(db, "source = \"Exchange trace\" AND ip = 192.0.2.8") == 1 && count(db, "ip = 192.0.2.9") == 1);
    CHECK(count(db, "field \"/FromIP\" = 192.0.2.8 AND field \"/ToIP\" = 192.0.2.9") == 1);
    // Directory audits name both sides.
    g_free(put("audit.json", "[{\"activityDateTime\":\"2026-06-01T00:00:00Z\",\"activityDisplayName\":\"Update user\","
                             "\"result\":\"success\",\"initiatedBy\":{\"user\":{\"userPrincipalName\":\"admin@example.org\","
                             "\"ipAddress\":\"192.0.2.1\"}},\"targetResources\":[{\"type\":\"User\",\"id\":\"t-1\","
                             "\"userPrincipalName\":\"target@example.org\"}]},"
                             "{\"activityDateTime\":\"2026-06-01T12:00:00Z\",\"activityDisplayName\":\"Consent to application\","
                             "\"result\":\"success\"}]"));
    CHECK(load(db, "audit.json", NULL) == IMPORT_DONE);
    CHECK(count(db, "ip = 192.0.2.1") == 1 && count(db, "target = \"target@example.org\"") == 1);
    CHECK(count(db, "mailbox = \"target@example.org\"") == 1 && count(db, "target = t-1 AND actor = \"admin@example.org\"") == 1);
    CHECK(count(db, "actor = \"target@example.org\"") == 0 && count(db, "target = \"admin@example.org\"") == 0);
    CHECK(report_total(db, "consent", "Consent to application") == 1);
    // Workload sign-ins have no user.
    g_free(put("workload.json", "{\"createdDateTime\":\"2026-06-01T12:00:00Z\",\"id\":\"w-1\",\"signInEventTypes\":[\"servicePrincipal\"],"
                                "\"servicePrincipalName\":\"Audit reader\",\"servicePrincipalId\":\"sp-2\","
                                "\"appDisplayName\":\"Audit reader\",\"status\":{\"errorCode\":0}}"));
    CHECK(load(db, "workload.json", NULL) == IMPORT_DONE);
    CHECK(count(db, "source = \"Graph sign-ins\" AND actor = \"Audit reader\" AND serviceprincipal = sp-2") == 1);
    CHECK(report_total(db, "outcomes", "Graph sign-ins") == 1);
    sqlite3_close(db);
}

int main(void) {
    root = g_dir_make_tmp("bec-test-XXXXXX", NULL);
    test_text();
    test_tabular();
    test_json();
    test_statistics();
    test_consistency();
    test_aliases();
    test_export_count();
    test_mark_scope();
    test_first_imports();
    test_permissions();
    test_read_only();
    test_copy();
    test_workbooks();
    test_handoff();
    test_pivots();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
