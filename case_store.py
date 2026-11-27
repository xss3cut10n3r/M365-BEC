"""Store and query case evidence."""

import contextlib
import csv
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import zipfile
import zlib

from bql import address, CASE_ID, REASON, VERDICT, Query, SOURCE, source_label, utc
from event_fields import sql_values, sql_match, match_path, field_display

csv.field_size_limit(64 * 1024 * 1024)
APP_ID = 0x42454331
DDL = """
CREATE TABLE files(id INTEGER PRIMARY KEY, path TEXT, digest TEXT, size INTEGER, UNIQUE(path,digest));
CREATE TABLE chunks(file INTEGER, part INTEGER, data BLOB, PRIMARY KEY(file,part));
CREATE TABLE logs(id INTEGER PRIMARY KEY, file INTEGER, name TEXT, sheet TEXT, kind TEXT,
                  headers TEXT, total INTEGER DEFAULT 0, issues INTEGER DEFAULT 0, warning TEXT DEFAULT '');
CREATE TABLE events(id INTEGER PRIMARY KEY, log INTEGER, rownum INTEGER, time TEXT, actor TEXT,
                    op TEXT, result TEXT, request TEXT, ip TEXT, raw TEXT, issue TEXT);
CREATE INDEX event_time ON events(time,id);
CREATE INDEX log_time ON events(log,time,id);
CREATE INDEX request_id ON events(request) WHERE request!='';
CREATE TABLE ips(event INTEGER, ip TEXT, PRIMARY KEY(event,ip));
CREATE INDEX ips_value ON ips(ip,event);
CREATE TABLE identities(event INTEGER, value TEXT COLLATE NOCASE, PRIMARY KEY(event,value));
CREATE INDEX identity_value ON identities(value,event);
CREATE TABLE marks(id INTEGER PRIMARY KEY,kind TEXT,lo TEXT,hi TEXT,UNIQUE(kind,lo,hi));
"""

VERDICTS = ("Good", "Bad", "Unsure", "Unmarked")
RULE_TABLE = """verdict_rules(id INTEGER PRIMARY KEY, batch INTEGER NOT NULL, kind TEXT NOT NULL,
    lo TEXT NOT NULL, hi TEXT NOT NULL, verdict TEXT NOT NULL)"""
TIME_ORDER = "(e.time=''),e.time,e.id"
TIMELINE_ROWS = f"SELECT e.id,row_number() OVER (ORDER BY {TIME_ORDER}) FROM events e"


def ensure_review_schema(db):
    if db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='trigger' AND name='refresh_timeline_ids'"
    ).fetchone():
        return
    with db:
        db.execute("BEGIN IMMEDIATE")
        db.execute("CREATE TABLE IF NOT EXISTS " + RULE_TABLE)
        db.execute("CREATE INDEX IF NOT EXISTS verdict_scope ON verdict_rules(kind,lo,id)")
        db.execute("CREATE INDEX IF NOT EXISTS verdict_batch ON verdict_rules(batch)")
        db.execute("CREATE INDEX IF NOT EXISTS chronological_events ON events((time=''),time,id)")
        db.execute(
            "CREATE TABLE IF NOT EXISTS timeline_ids(event INTEGER PRIMARY KEY, number INTEGER UNIQUE)"
        )
        db.execute("DELETE FROM timeline_ids")
        db.execute("INSERT INTO timeline_ids " + TIMELINE_ROWS)
        # Renumber after each completed sheet.
        db.execute(f"""CREATE TRIGGER IF NOT EXISTS refresh_timeline_ids AFTER UPDATE OF total ON logs
            WHEN NEW.total != OLD.total BEGIN
            DELETE FROM timeline_ids;
            INSERT INTO timeline_ids {TIMELINE_ROWS};
            END""")


def assign_verdict(db, kind, targets, verdict):
    if kind not in ("event", "ip", "time") or verdict not in VERDICTS:
        raise ValueError("Unknown verdict or scope")
    with db:
        db.execute("BEGIN IMMEDIATE")
        batch = db.execute("SELECT coalesce(max(batch),0)+1 FROM verdict_rules").fetchone()[0]
        db.executemany(
            "INSERT INTO verdict_rules(batch,kind,lo,hi,verdict) VALUES(?,?,?,?,?)",
            ((batch, kind, str(lo), hi, verdict) for lo, hi in targets),
        )


def undo_verdict(db):
    with db:
        db.execute("BEGIN IMMEDIATE")
        removed = db.execute("DELETE FROM verdict_rules WHERE batch=(SELECT max(batch) FROM verdict_rules)")
        if not removed.rowcount:
            db.execute("DELETE FROM marks WHERE id=(SELECT max(id) FROM marks)")


def connect(path):
    db = sqlite3.connect(path, timeout=30)
    try:
        if db.execute("PRAGMA application_id").fetchone()[0] != APP_ID:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT count(*) FROM sqlite_master").fetchone()[0]:
                raise ValueError("Choose a valid M365 BEC case file")
            for statement in DDL.split(";"):
                if statement.strip():
                    db.execute(statement)
            db.execute(f"PRAGMA application_id={APP_ID}")
            db.execute("PRAGMA user_version=2")
            db.commit()
        if db.execute("PRAGMA user_version").fetchone()[0] == 1:
            with db:
                db.execute("BEGIN IMMEDIATE")
                db.execute("ALTER TABLE files RENAME TO previous_files")
                db.execute(DDL.split(";")[0])
                db.execute("INSERT INTO files SELECT id,path,digest,size FROM previous_files")
                db.execute("DROP TABLE previous_files")
                db.execute("PRAGMA user_version=2")
        if db.execute("PRAGMA user_version").fetchone()[0] != 2:
            raise ValueError("Unsupported case format")
        ensure_review_schema(db)
        configure_reader(db)
        db.execute("PRAGMA journal_mode=WAL")
        return db
    except BaseException:
        db.close()
        raise


def configure_reader(db):
    db.row_factory = sqlite3.Row
    db.create_function("source_label", 1, source_label, deterministic=True)
    db.create_function("event_values", 3, sql_values, deterministic=True)
    db.create_function("event_match", 5, sql_match, deterministic=True)
    db.create_function("match_path", 5, match_path, deterministic=True)
    db.create_function("field_display", 3, field_display, deterministic=True)
    db.create_collation(
        "BQL_TEXT", lambda a, b: (a.casefold() > b.casefold()) - (a.casefold() < b.casefold())
    )


def read_case(path):
    db = sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)
    if not db.execute("SELECT 1 FROM sqlite_master WHERE name='verdict_rules'").fetchone():
        db.execute("CREATE TEMP TABLE " + RULE_TABLE)
        with db:
            db.execute("CREATE TEMP TABLE timeline_ids(event INTEGER PRIMARY KEY, number INTEGER UNIQUE)")
            db.execute("INSERT INTO timeline_ids " + TIMELINE_ROWS)
    configure_reader(db)
    return db


def packed(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def text(value):
    if value is None:
        return ""
    if isinstance(value, (dict, list)):
        return packed(value)
    if isinstance(value, (dt.date, dt.time)):
        return value.isoformat()
    return str(value)


def clean_header(v):
    return " ".join(v.casefold().split())


def detect(headers, name):
    h = set(map(clean_header, headers))
    name = name.casefold()
    if "auditdata" in h:
        return "Purview audit"
    if "actortype" in h and "activity" in h:
        return "Entra directory audit"
    if "request id" in h:
        if "noninteractive" in name or "non-interactive" in name:
            family = "Non-interactive"
        elif "interactive" in name:
            family = "Interactive"
        elif "service principal id" in h and "username" not in h:
            family = (
                "Managed identity"
                if "msisignins" in name
                else "Application" if "applicationsignins" in name else "Workload"
            )
        else:
            family = "User"
        return family + (
            " auth details" if "authentication method" in h and "succeeded" in h else " sign-ins"
        )
    if (
        {"message_id", "sender_address"} <= h
        or {"recipientaddress", "senderaddress"} <= h
        or {"recipient address", "sender address"} <= h
    ):
        return "Exchange trace"
    if {"source", "source row", "operation", "time (utc)"} <= h:
        return "Timeline"
    return "Tabular log"


@contextlib.contextmanager
def sheets(path, extension):
    if extension == ".xlsx":
        from openpyxl import load_workbook

        with zipfile.ZipFile(path) as archive:
            if sum(f.file_size for f in archive.infolist()) > 2 * 1024**3:
                raise ValueError("Workbook exceeds the 2 GiB expanded import limit")
        with open(path, "rb") as stream:
            book = load_workbook(stream, read_only=True, data_only=False, keep_links=False)
            try:
                for sheet in book:
                    sheet.reset_dimensions()
                yield ((s.title, s.values) for s in book)
            finally:
                book.close()
        return
    if extension != ".csv":
        raise ValueError("Choose CSV or XLSX")
    with open(path, "rb") as buf:
        prefix = buf.read(65536)
        buf.seek(0)
        encoding = "utf-16" if prefix.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        stream = io.TextIOWrapper(buf, encoding=encoding, newline="")
        sample = stream.readline()
        stream.seek(0)
        try:
            delimiter = csv.Sniffer().sniff(sample, delimiters=",;\t").delimiter
        except csv.Error:
            delimiter = ","
        yield [("", csv.reader(stream, delimiter=delimiter, strict=True))]


def import_sheet(db, file_id, name, sheet, rows, progress):
    headers = [text(s) for s in next(rows, ())]
    if not headers:
        return
    kind = detect(headers, name + " " + sheet)
    positions = {}
    for i, h in enumerate(headers):
        positions.setdefault(clean_header(h), i)
    target_cols = [
        i for i, h in enumerate(headers) if re.fullmatch(r"target\d+userprincipalname", clean_header(h))
    ]
    log = db.execute(
        "INSERT INTO logs(file,name,sheet,kind,headers) VALUES(?,?,?,?,?)",
        (file_id, name, sheet, kind, packed(headers)),
    ).lastrowid
    count = problems = 0
    for count, row in enumerate(rows, 1):
        values = [text(s) for s in row]

        def get(*keys):
            for k in keys:
                idx = positions.get(k)
                if idx is not None and idx < len(values) and values[idx]:
                    return values[idx]
            return ""

        notes = []
        if len(values) != len(headers):
            notes.append(f"Expected {len(headers)} fields, found {len(values)}")
        audit = {}
        if kind == "Purview audit":
            try:
                audit = json.loads(get("auditdata"))
                if not isinstance(audit, dict):
                    raise ValueError()
            except (ValueError, RecursionError):
                notes.append("Malformed AuditData")
                audit = {}
        actor = text(audit.get("UserId")) or get(
            "username",
            "userid",
            "actoruserprincipalname",
            "actorserviceprincipalname",
            "actordisplayname",
            "service principal name",
            "sender_address",
            "senderaddress",
            "sender address",
            "mailbox",
            "actor",
        )
        stamp = text(audit.get("CreationTime")) or get(
            "date (utc)",
            "creationdate",
            "date",
            "date_time",
            "origin_timestamp",
            "received",
            "time (utc)",
            "timestamp",
        )
        time = utc(stamp)
        if stamp and not time:
            notes.append("Unrecognized timestamp")
        op = (
            text(audit.get("Operation"))
            or get("operation", "activity", "authentication method", "event_id", "event")
            or kind
        )
        result = text(audit.get("ResultStatus")) or get("status", "result", "succeeded", "recipient_status")
        request = get("request id").strip().lower()
        raw_ips = [audit.get(k) for k in ("ClientIP", "ClientIPAddress", "ActorIpAddress")]
        raw_ips += [
            get(k)
            for k in (
                "ip address",
                "ipaddress",
                "ip address (seen by resource)",
                "global secure access ip address",
                "original_client_ip",
                "client_ip",
                "ip",
            )
        ]
        ips = list(dict.fromkeys(ip for v in raw_ips if v for ip in [address(v)] if ip))
        users = [actor, text(audit.get("MailboxOwnerUPN")), get("actoruserprincipalname"), get("username")]
        users.extend(values[i] for i in target_cols if i < len(values))
        for recipient in get(
            "recipient_address", "recipientaddress", "recipient address", "recipient_status"
        ).split(";"):
            users.append(recipient.split("##")[0])
        eid = db.execute(
            "INSERT INTO events(log,rownum,time,actor,op,result,request,ip,raw,issue) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (
                log,
                count + 1,
                time,
                actor,
                op,
                result,
                request,
                ", ".join(ips),
                packed(values),
                ", ".join(notes),
            ),
        ).lastrowid
        db.executemany("INSERT INTO ips VALUES(?,?)", ((eid, ip) for ip in ips))
        db.executemany(
            "INSERT OR IGNORE INTO identities VALUES(?,?)", ((eid, u.strip()) for u in users if u.strip())
        )
        problems += bool(notes)
        if count % 5000 == 0:
            progress(f"{name} · {count:,} rows")
    cap = (
        100000
        if kind.endswith((" sign-ins", " auth details"))
        else 250000 if kind == "Entra directory audit" else None
    )
    warning = (
        f"Possible truncation: {count:,} records (portal limit {cap:,})." if cap and count >= cap else ""
    )
    db.execute("UPDATE logs SET total=?,issues=?,warning=? WHERE id=?", (count, problems, warning, log))


def import_file(db, path, progress=lambda message: None):
    """Import each evidence file atomically."""
    path = Path(path).resolve()
    if path.suffix.lower() not in (".csv", ".xlsx"):
        raise ValueError("Unsupported format")
    with tempfile.TemporaryDirectory() as folder:
        snapshot = Path(folder) / ("input" + path.suffix)
        digest = hashlib.sha256()
        with path.open("rb") as src, snapshot.open("wb") as dst:
            while chunk := src.read(1024 * 1024):
                dst.write(chunk)
                digest.update(chunk)
        sha = digest.hexdigest()
        with db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT 1 FROM files WHERE path=? AND digest=?", (str(path), sha)).fetchone():
                return "Already imported"
            fid = db.execute(
                "INSERT INTO files(path,digest,size) VALUES(?,?,?)",
                (str(path), sha, snapshot.stat().st_size),
            ).lastrowid
            with snapshot.open("rb") as src:
                part = 0
                while chunk := src.read(1024 * 1024):
                    db.execute("INSERT INTO chunks VALUES(?,?,?)", (fid, part, zlib.compress(chunk, 1)))
                    part += 1
            with sheets(snapshot, path.suffix.lower()) as tables:
                for title, rows in tables:
                    import_sheet(db, fid, path.name, title, iter(rows), progress)
            if not db.execute("SELECT 1 FROM logs WHERE file=?", (fid,)).fetchone():
                raise ValueError("File contains no header row")
    return "Imported"


BASE_COLUMNS = f"""e.id,e.rownum,e.time,e.actor,e.op,e.result,e.ip,e.issue,l.kind,l.name,l.sheet,
             {SOURCE} AS source,
             {CASE_ID} AS timeline_id,{VERDICT} AS verdict,{REASON} AS reason"""
EVENT_JOIN = " FROM events e JOIN logs l ON l.id=e.log"
SORTS = {
    "rownum": "e.rownum",
    "time": "e.time",
    "source": SOURCE,
    "name": "l.name",
    "actor": "e.actor",
    "ip": "e.ip",
    "op": "e.op",
    "result": "e.result",
    "timeline_id": CASE_ID,
    "marks": VERDICT,
}


def select_sql(custom=()):
    columns = [BASE_COLUMNS] + [
        f"field_display(l.headers,e.raw,?) AS custom_{i}" for i, _ in enumerate(custom)
    ]
    return "SELECT " + ",".join(columns) + EVENT_JOIN, [path for label, path in custom]


def sort_sql(key, desc):
    order = "DESC" if desc else "ASC"
    if key in ("time", "timeline_id"):
        missing_order = order if key == "timeline_id" else "ASC"
        return f" ORDER BY (e.time='') {missing_order}, e.time {order}, e.id {order}", []
    expr, params = (
        ("field_display(l.headers,e.raw,?)", [key[4:]]) if key.startswith("ptr:") else (SORTS[key], [])
    )
    return f" ORDER BY {expr} {order}, e.id {order}", params


def where(log, query):
    q = Query(query)
    return " WHERE " + q.sql + (" AND e.log=?" if log else ""), q.args + ([log] if log else [])


def query_rows(db, log, query, desc=False, limit=None, offset=0, *, order_by="time", custom=()):
    select, projection = select_sql(custom)
    clause, filters = where(log, query)
    order, ordering = sort_sql(order_by, desc)
    sql = select + clause + order
    args = projection + filters + ordering
    if limit is not None:
        sql += " LIMIT ? OFFSET ?"
        args.extend([limit, offset])
    return db.execute(sql, args)


def count_rows(db, log, query):
    clause, args = where(log, query)
    return db.execute("SELECT count(*) FROM events e JOIN logs l ON e.log=l.id" + clause, args).fetchone()[
        0
    ]


def mark_label(row):
    return row["reason"]


EXPORT_HEADER = [
    "Source",
    "File",
    "Sheet",
    "Source row",
    "Time (UTC)",
    "Actor",
    "Operation",
    "Result",
    "IP",
    "Analyst mark",
    "Import issue",
    "ID",
    "Verdict",
]


def export(db, dest, log, query, desc=False, *, order_by="time", custom=()):
    dest = Path(dest).resolve()
    guard_destination(db, dest)

    def records():
        for r in query_rows(db, log, query, desc, order_by=order_by, custom=custom):
            yield [
                r["source"],
                r["name"],
                r["sheet"],
                r["rownum"],
                r["time"],
                r["actor"],
                r["op"],
                r["result"],
                r["ip"],
                (r["verdict"] + " · " + r["reason"]) if r["reason"] else "",
                r["issue"],
                r["timeline_id"],
                r["verdict"],
                *(r[f"custom_{i}"] for i in range(len(custom))),
            ]

    with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False, suffix=dest.suffix) as tmp:
        temp = Path(tmp.name)
    try:
        if dest.suffix.lower() == ".xlsx":
            from openpyxl import Workbook
            from openpyxl.cell import WriteOnlyCell

            book = Workbook(write_only=True)
            sheet = book.create_sheet("Timeline")
            headings = EXPORT_HEADER + ["Field " + path for _, path in custom]
            sheet.append(headings)
            for idx, row in enumerate(records(), 1):
                if idx > 1 and (idx - 1) % 1048575 == 0:
                    sheet = book.create_sheet()
                    sheet.append(headings)
                cells = []
                for value in row:
                    if isinstance(value, str) and len(value) > 32767:
                        raise ValueError("Value exceeds Excel cell limit. Export CSV instead.")
                    cell = WriteOnlyCell(sheet, value=value)
                    if isinstance(value, str):
                        cell.data_type = "s"
                    cells.append(cell)
                sheet.append(cells)
            book.save(temp)
        elif dest.suffix.lower() == ".csv":
            with temp.open("w", newline="", encoding="utf-8-sig") as out:
                writer = csv.writer(out)
                writer.writerow(EXPORT_HEADER + ["Field " + path for _, path in custom])
                for row in records():
                    writer.writerow(
                        [
                            (
                                "'" + v
                                if isinstance(v, str) and v.lstrip().startswith(("=", "+", "-", "@"))
                                else v
                            )
                            for v in row
                        ]
                    )
        else:
            raise ValueError("Choose CSV or XLSX")
        os.replace(temp, dest)
    finally:
        temp.unlink(missing_ok=True)


def guard_destination(db, dest):
    originals = [r[0] for r in db.execute("SELECT path FROM files")]
    case = db.execute("PRAGMA database_list").fetchone()[2]
    protected = originals + [case, case + "-wal", case + "-shm", case + "-journal"]
    if Path(dest).resolve() in {Path(p).resolve() for p in protected}:
        raise ValueError("Choose a destination separate from evidence and case files")


def extract(db, file_id, destination):
    destination = Path(destination).resolve()
    guard_destination(db, destination)
    wanted = db.execute("SELECT digest FROM files WHERE id=?", (file_id,)).fetchone()[0]
    digest = hashlib.sha256()
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as out:
        temp = Path(out.name)
        try:
            for (chunk,) in db.execute("SELECT data FROM chunks WHERE file=? ORDER BY part", (file_id,)):
                block = zlib.decompress(chunk)
                out.write(block)
                digest.update(block)
            if digest.hexdigest() != wanted:
                raise ValueError("Archived bytes failed SHA-256 verification")
            out.close()
            os.replace(temp, destination)
        finally:
            out.close()
            temp.unlink(missing_ok=True)
