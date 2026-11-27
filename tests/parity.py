"""Compare the C port with Python."""

import csv
import json
from pathlib import Path
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import bql
import case_store as store
from event_fields import SLOTS
import telemetry
from make_fixtures import small

TOOL = Path(sys.argv[1]).resolve()
failures = []


def check(name, expected, actual):
    if expected != actual:
        failures.append(name)
        print("FAIL", name)
        if isinstance(expected, list) and isinstance(actual, list):
            for i, (a, b) in enumerate(zip(expected, actual)):
                if a != b:
                    print("   first difference at", i, "\n   python:", repr(a)[:600], "\n   c     :", repr(b)[:600])
                    break
            else:
                print("   lengths", len(expected), len(actual))
        else:
            print("   python:", repr(expected)[:600], "\n   c     :", repr(actual)[:600])
    else:
        print("ok  ", name)


def tool(*args, stdin=None):
    out = subprocess.run([str(TOOL), *map(str, args)], input=stdin, capture_output=True, encoding="utf-8")
    return [json.loads(line) if line.startswith("[") else line for line in out.stdout.splitlines()]


def batch(mode, values):
    return tool(mode, stdin="".join(json.dumps(v) + "\n" for v in values))


def arg(folder, text):
    path = folder / "arg.txt"
    path.write_text(text, encoding="utf-8")
    return "@" + str(path)


def safe_utc(value):
    # Python overflows near year limits.
    try:
        return bql.utc(value)
    except OverflowError:
        return ""


def fuzz():
    rng = random.Random(11)
    stamps = [
        "2026-01-02", "2026-01-02T11:04:00", "2026-01-02T11:04:00Z", "2026-01-02 11:04:00.5+05:30", "20260102",
        "20260102T110400", "2026-W01-1", "2026W011", "2026-W01", "2026-01-02T11", "2026-01-02T11:04", "2026-1-2",
        "2026-01-02T11:04:00.1234567Z", "2026-01-02T11:04:00,5", "2026-01-02T24:00:00", "2026-02-30", "0001-01-01",
        "9999-12-31T23:59:59.999999", "9999-12-31T23:59:59-01:00", "0001-01-01T00:00:00+01:00", " 2026-01-02 ",
        "2026-01-02T11:04:00+0530", "2026-01-02T11:04:00+05", "2026-01-02T11:04:00-23:59:59.5", "2026-01-02T11:04:00+24:00",
        "2026-01-02x11:04", "2026-01-02T1104", "2026-01-02T11:0400", "1/2/2026 11:04", "", "Z", "2026-W53-1", "2020-W53-7",
        "2026-01-02T11:04:00.", "2026-01-02T11:04:60", "2026-13-01", "2026-01-02é11:04", "2026-01-02T11:04:00 Z",
        "2026-01-02T11:04:00+00:00:00.000001", "20260102T11", "2026W01", "2026-W01-1T10", "2026001", "2026-001",
    ]
    alphabet = "0123456789-:TZ+., W"
    for _ in range(4000):
        base = list(rng.choice(stamps[:25]))
        for _ in range(rng.randrange(3)):
            if base:
                i = rng.randrange(len(base))
                base[i : i + rng.randrange(2)] = rng.choice(alphabet)
        stamps.append("".join(base))
    check("timestamp normalization", [[safe_utc(s)] for s in stamps], batch("utc", stamps))

    ips = [
        "192.0.2.8", "192.0.2.8:443", "[2001:db8::1]:443", "[2001:db8::1]", "2001:DB8:0:0:0:0:0:9", "::1", "::", "1::",
        "::ffff:1.2.3.4", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1::8", "01.2.3.4", "256.1.1.1",
        "1.2.3", "fe80::1%eth0", "fe80::1%", "[fe80::1%eth0]:80", "1:2:3:4:5:6:1.2.3.4", "::1.2.3.4", "1:2:3:4:5:6:7:8:9",
        "12345::", " 10.0.0.1 ", "10.0.0.1:", "10.0.0.1:x", "[::1]:x", "[]", "bad", "", "1:0:0:2:0:0:0:3", "0:0:1:0:0:1:0:0",
        "::ffff:0:0", "64:ff9b::192.0.2.33", ":1", "1:", ":::", "1::2::3", "A:b:C:d::", "1.2.3.4.5", "0.0.0.0", "1..2.3",
    ]
    pieces = ["1", "0", "ffff", "::", ":", "db8", "2001", ".", "%", "a", "[", "]", "192", "g", "00"]
    for _ in range(4000):
        ips.append("".join(rng.choice(pieces) for _ in range(rng.randrange(1, 12))))
    check("ip normalization", [[bql.address(s)] for s in ips], batch("ip", ips))

    headers = ["Date (UTC)", "  Request\tID ", "ÄÖ Straße", "İstanbul", "a b", "X Y", "", "ﬁeld"]
    check("header cleaning", [[store.clean_header(h)] for h in headers], batch("header", headers))

    words = ["operation", "=", "!=", "contains", "AND", "OR", "NOT", "(", ")", '"Send"', "ip", "192.0.2.8", "time",
             "2026-01-02", ">=", "<", "marked", "true", "false", "row", "5", "id", "field", '"/a/b"', "exists",
             "mailbox", "source", '"sign-ins"', "session", "verdict", "Bad", '"x\\"y"', "==", "<=", ">", "raw", "bogus",
             "!", '"bad\\q"', "99999999999999999999", "request", "markreason", "sha256", "path", "file", "sheet", "issue"]
    filters = ["", "  ", 'operation = "Send"', "ip = broken OR", 'field "/AuditData/Data~1~0/*/x" = "false"',
               'exists "/AuditData"', 'exists "nope"', "time >= 2026-01-02", "time contains 2026", "row = 5 AND id != 3",
               'NOT (marked = false)', "marked != true", 'source = "interactive sign-ins"', "ip contains 192.0",
               'ip = "[2001:db8::1]:443"', "device > 5", 'field "/a" < "b"', "operation", "(operation = a", "a = b c",
               'mailbox != "X@example.org"', "id = 9223372036854775808", "row contains 5", 'actor = "tab\there"']
    for _ in range(3000):
        filters.append(" ".join(rng.choice(words) for _ in range(rng.randrange(1, 9))))
    expected = []
    for text in filters:
        try:
            q = bql.Query(text)
            expected.append(["ok", *map(str, q.args)])
        except ValueError as exc:
            expected.append(["error", str(exc)])
        except OverflowError:
            expected.append(["error", "Compare time with an ISO date or timestamp"])
    actual = batch("bql", filters)
    # JSON errors are worded differently.
    for e, a in zip(expected, actual):
        if e[0] == a[0] == "error" and ("Invalid" in a[1] and "quoted" in a[1]):
            a[1] = e[1]
    check("BQL compilation", expected, actual)


FILTERS = [
    "", 'operation = "Send"', 'operation contains "inboxrule"', 'mailbox = "ANALYST@example.org"', "ip = 203.0.113.7",
    'ip = "2001:db8::9" OR ip = "2001:db8::7"', "ip contains 198.51", 'source = "Purview audit"',
    'source contains "sign-ins"', 'source = "interactive sign-ins"', "time >= 2026-01-02T11:00:00Z AND time < 2026-01-03",
    'session = "session-3"', 'device != "device-1"', 'recipient contains "vendor"', 'delivery = "Receive, Fail"',
    'rule contains "invoice"', 'folder = "\\\\Inbox"', 'mailitem contains "item-"', 'access = "Sync"',
    'target = "obj-t3"', 'subject contains "wire"', 'messageid = "<t3@example.org>"', 'workload = "Exchange"',
    'field "/AuditData/Parameters/*/Name" = "MoveToFolder"', 'field "/AuditData/RecordType" = "2"',
    'field "/@2:Incoming token type" = "Primary Refresh Token"', 'exists "/AuditData/Item"', 'NOT exists "/Status"',
    'field "/Target1ModifiedProperty1NewValue/*/Scope" contains "mail"', 'issue contains "Malformed"', "row <= 3",
    "id > 190", 'file = "MessageTrace.csv"', 'sheet = "Notes"', 'result = "failure" AND NOT operation contains "sign"',
    'raw contains "Zoë"', 'actor = "zoë.müller@example.org"', 'request = "req-0004"', "marked = true", "marked = false",
    'verdict = "Bad"', 'verdict = Good OR verdict = Unsure', 'markreason = "IP"', 'correlation = "corr-3"',
    'application contains "exchange"', 'os = "MacOs"', 'location contains "lagos"', 'conditionalaccess = "Failure"',
    'authmethod = "Password"', 'operation = "=1+1"',
]


def rows(db, log, query, desc=False, order="time", custom=()):
    return [["" if v is None else str(v) for v in tuple(r)] for r in store.query_rows(db, log, query, desc, order_by=order, custom=custom)]


def table(db, sql):
    return [list(r) for r in db.execute(sql)]


def compare_cases(label, py, c):
    for name, sql in [
        ("files", "SELECT id,path,digest,size FROM files ORDER BY id"),
        ("logs", "SELECT id,file,name,sheet,kind,headers,total,issues,warning FROM logs ORDER BY id"),
        ("events", "SELECT id,log,rownum,time,actor,op,result,request,ip,raw,issue FROM events ORDER BY id"),
        ("ips", "SELECT event,ip FROM ips ORDER BY event,ip"),
        ("identities", "SELECT event,value FROM identities ORDER BY event,value"),
        ("timeline ids", "SELECT event,number FROM timeline_ids ORDER BY event"),
        ("verdict rules", "SELECT id,batch,kind,lo,hi,verdict FROM verdict_rules ORDER BY id"),
    ]:
        check(f"{label}: {name}", table(py, sql), table(c, sql))
    blobs = [[zlib.decompress(r[0]) for r in db.execute("SELECT data FROM chunks ORDER BY file,part")] for db in (py, c)]
    check(f"{label}: archived evidence bytes", blobs[0], blobs[1])


def compare_queries(label, py, c_path, folder):
    logs = [0] + [r[0] for r in py.execute("SELECT id FROM logs")]
    for query in FILTERS:
        expected = rows(py, 0, query)
        check(f"{label}: rows {query!r}", expected, tool("rows", c_path, 0, arg(folder, query), "time", 0))
    for order in ("rownum", "source", "name", "actor", "ip", "op", "result", "timeline_id", "marks", "ptr:/Username"):
        for desc in (False, True):
            check(f"{label}: sort {order} desc={desc}", rows(py, 0, "", desc, order),
                  tool("rows", c_path, 0, arg(folder, ""), order, int(desc)))
    custom = (("Subject", "/AuditData/Item/Subject"), ("Token", "/@2:Incoming token type"), ("Scope", "/Target1ModifiedProperty1NewValue/*/Scope"))
    check(f"{label}: custom columns", rows(py, 0, "", custom=custom),
          tool("rows", c_path, 0, arg(folder, ""), "time", 0, *(p for _, p in custom)))
    for log in logs[:4]:
        for query in ("", 'result = "Success"', "marked = true"):
            for key, report in telemetry.REPORTS.items():
                for interval in (3600,) if key != "activity" else (60, 900, 3600, 86400):
                    expected = [["" if v is None else str(v) for v in r] for r in telemetry.calculate(py, key, log, query, interval)]
                    actual = tool("stats", c_path, key, log, arg(folder, query), interval)
                    actual = [["" if v is None else v for v in r] for r in actual]
                    check(f"{label}: statistics {key} log={log} {query!r} {interval}", expected, actual)
    for key in telemetry.REPORTS:
        result = tool("drill", c_path, key, 0, arg(folder, ""), 3600)
        check(f"{label}: drill-down {key}", [], [r for r in result if not str(r).startswith("checked")])
    expected = [[r[1], r[3], str(r[4]), str(r[5]), None if r[7] is None else str(r[7]), None if r[8] is None else str(r[8]), r[6]] for r in telemetry.quality(py)]
    check(f"{label}: import quality", expected, tool("quality", c_path))


def main():
    fuzz()
    folder = Path(tempfile.mkdtemp(prefix="bec-parity-"))
    small(folder)
    names = ["MailboxAudit.csv", "InteractiveSignIns_2026-01-02.csv", "InteractiveSignIns_AuthDetails_2026-01-02.csv",
             "AuditLogs_2026-01-03.csv", "MessageTrace.csv", "recurrence-review.xlsx", "malformed.csv"]
    (folder / "tabs.csv").write_text("Date\tUser;Name\tOperation\n2026-01-01\t\"a;b\"\tRead\n\n\"x\"\"y\",\n", encoding="utf-8")
    (folder / "quoted.csv").write_text('"Date (UTC)";"Username";"Operation"\r\n"2026-01-01";"q@example.org";"multi\r\nline"\r"2026-01-02";"r@example.org";"cr only"', encoding="utf-8")
    (folder / "empty.csv").write_text("", encoding="utf-8")
    (folder / "latin.csv").write_bytes(b"Operation\nCaf\xe9\n")
    (folder / "strict.csv").write_text('Operation,Actor\n"Send"x,a\n', encoding="utf-8")
    names += ["tabs.csv", "quoted.csv", "empty.csv", "latin.csv", "strict.csv"]
    py_case, c_case = folder / "python.bec", folder / "c.bec"
    py = store.connect(py_case)
    states = []
    for name in names + names[:2]:
        try:
            states.append([name, store.import_file(py, folder / name)])
        except Exception:
            states.append([name, "Failed"])
    actual = tool("import", c_case, *(folder / n for n in names + names[:2]))
    check("import outcomes", states, [r[:2] for r in actual])
    print("    C import errors:", [(r[0], r[2]) for r in actual if r[2]])
    c = sqlite3.connect(c_case)
    compare_cases("import", py, c)
    c.close()
    compare_queries("unmarked", py, c_case, folder)

    # Apply identical analyst decisions.
    with py:
        py.execute("INSERT INTO marks(kind,lo,hi) VALUES('ip','192.0.2.44','')")
        py.execute("INSERT INTO marks(kind,lo,hi) VALUES('time',?,?)", (bql.utc("2026-01-02T11:00"), bql.utc("2026-01-02T11:10")))
    c = sqlite3.connect(c_case)
    with c:
        c.execute("INSERT INTO marks(kind,lo,hi) VALUES('ip','192.0.2.44','')")
        c.execute("INSERT INTO marks(kind,lo,hi) VALUES('time',?,?)", (bql.utc("2026-01-02T11:00"), bql.utc("2026-01-02T11:10")))
    c.close()
    steps = [("ip", "Bad", [("203.0.113.7", "")]), ("event", "Good", [(5, ""), (6, ""), (70, "")]),
             ("time", "Unsure", [(bql.utc("2026-01-02T09:00"), bql.utc("2026-01-02T09:30"))]), ("event", "Unmarked", [(5, "")])]
    for kind, verdict, targets in steps:
        store.assign_verdict(py, kind, targets, verdict)
        tool("verdict", c_case, kind, verdict, *(x for pair in targets for x in pair))
    compare_queries("marked", py, c_case, folder)

    for ext in ("csv", "xlsx"):
        for i, (query, order, desc, custom) in enumerate([("", "time", False, ()), ('verdict = "Bad"', "op", True, (("Subject", "/AuditData/Item/Subject"),))]):
            a, b = folder / f"py-{i}.{ext}", folder / f"c-{i}.{ext}"
            store.export(py, a, 0, query, desc, order_by=order, custom=custom)
            tool("export", c_case, b, 0, arg(folder, query), order, int(desc), *(p for _, p in custom))
            if ext == "csv":
                check(f"export {ext} {i} bytes", a.read_bytes(), b.read_bytes())
            else:
                from openpyxl import load_workbook
                cells = []
                for path in (a, b):
                    book = load_workbook(path, read_only=True)
                    # Carriage returns are escaped differently.
                    cells.append([[(c.value.replace("_x000D_", chr(10)) if isinstance(c.value, str) else c.value, c.data_type if c.value is not None else "") for c in row] for sheet in book for row in sheet.rows])
                    book.close()
                check(f"export {ext} {i} cells", cells[0], cells[1])
    for file_id, name in py.execute("SELECT id,(SELECT name FROM logs WHERE file=files.id LIMIT 1) FROM files").fetchall():
        out = folder / ("copy-" + name)
        tool("extract", c_case, file_id, out)
        check(f"recovered evidence {name}", (folder / name).read_bytes(), out.read_bytes() if out.exists() else None)
    check("evidence overwrite guard", True, "ERROR" in str(tool("extract", c_case, 1, folder / names[0])))

    # Reimport exports in both.
    for ext in ("csv", "xlsx"):
        try:
            state = store.import_file(py, folder / f"c-0.{ext}")
        except Exception as exc:
            state = "Failed " + str(exc)
        check(f"python reimports C {ext} export", "Imported", state)
        check(f"C reimports its {ext} export", "Imported", tool("import", c_case, folder / f"c-0.{ext}")[0][1])
    c = sqlite3.connect(c_case)
    compare_cases("after reimport", py, c)
    c.close()
    for kind, _, _ in steps + [("legacy", 0, 0)]:
        store.undo_verdict(py)
        tool("undo", c_case)
        check(f"undo {kind}", rows(py, 0, "marked = true"), tool("rows", c_case, 0, "marked = true", "time", 0))
    py.close()

    # Each app opens the other's case.
    shutil.copy(c_case, folder / "c-copy.bec")
    shutil.copy(py_case, folder / "py-copy.bec")
    reopened = store.connect(folder / "c-copy.bec")
    check("python reopens C case", tool("rows", c_case, 0, "", "time", 0), rows(reopened, 0, ""))
    extra = folder / "later.csv"
    extra.write_text("Date (UTC),Username,Operation\n2026-01-09,late@example.org,Send\n", encoding="utf-8")
    check("python imports into C case", "Imported", store.import_file(reopened, extra))
    store.assign_verdict(reopened, "event", [(1, "")], "Bad")
    check("python marks C case", 1, store.count_rows(reopened, 0, 'markreason = "Event"'))
    out = folder / "from-c-case.csv"
    store.extract(reopened, 1, out)
    check("python recovers evidence from C case", (folder / names[0]).read_bytes(), out.read_bytes())
    check("C case keeps format identifiers", [store.APP_ID, 2],
          [reopened.execute("PRAGMA application_id").fetchone()[0], reopened.execute("PRAGMA user_version").fetchone()[0]])
    reopened.close()
    check("C reopens python case", [str(r[0]) for r in sqlite3.connect(py_case).execute("SELECT count(*) FROM events")],
          tool("count", folder / "py-copy.bec", 0, ""))

    # Version 1 cases migrate.
    old = folder / "v1.bec"
    v1 = sqlite3.connect(old)
    for statement in store.DDL.split(";"):
        if statement.strip():
            v1.execute(statement.replace("UNIQUE(path,digest)", "extra TEXT"))
    v1.execute(f"PRAGMA application_id={store.APP_ID}")
    v1.execute("PRAGMA user_version=1")
    v1.execute("INSERT INTO files(id,path,digest,size,extra) VALUES(1,'x.csv','00',3,'old')")
    v1.commit()
    v1.close()
    shutil.copy(old, folder / "v1-c.bec")
    tool("count", folder / "v1-c.bec", 0, "")
    migrated = sqlite3.connect(folder / "v1-c.bec")
    check("version 1 migration", [2, [(1, "x.csv", "00", 3)]],
          [migrated.execute("PRAGMA user_version").fetchone()[0], migrated.execute("SELECT * FROM files").fetchall()])
    migrated.close()
    junk = folder / "not-a-case.bec"
    junk_db = sqlite3.connect(junk)
    junk_db.execute("CREATE TABLE other(x)")
    junk_db.commit()
    junk_db.close()
    check("foreign database rejected", ["ERROR\tChoose a valid M365 BEC case file"], tool("count", junk, 0, ""))

    print()
    print("FAILED:" if failures else "ALL PARITY CHECKS PASSED", *failures, sep="\n  " if failures else " ")
    shutil.rmtree(folder, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
