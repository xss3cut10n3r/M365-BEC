"""Aggregate filtered case evidence."""

from dataclasses import dataclass
from datetime import datetime, timezone
import json

import case_store as store
from bql import SOURCE
from event_fields import SLOTS, columns, supports


def quote(value):
    return json.dumps(str(value), ensure_ascii=False)


def combine(first, second):
    return f"({first}) AND ({second})" if first and second else first or second


@dataclass(frozen=True)
class Report:
    title: str
    fields: tuple
    headings: tuple
    subset: str = ""


REPORTS = {
    "activity": Report("Activity over time", ("time",), ("Interval start (UTC)",)),
    "indicators": Report("Investigation indicators", ("indicator",), ("Recorded activity",)),
    "sources": Report("Log sources", ("source",), ("Source",)),
    "files": Report("Evidence files", ("file",), ("Filename",)),
    "operations": Report("Operations", ("operation",), ("Operation",)),
    "endpoints": Report("IP endpoints", ("ip",), ("IP address",)),
    "mailboxes": Report("Accounts and mailboxes", ("mailbox",), ("Identity",)),
    "conversations": Report(
        "Mail conversations",
        ("actor", "recipient"),
        ("Sender", "Recipient"),
        'operation = "Send" OR operation = "SendAs" OR operation = "SendOnBehalf" OR source = "Exchange trace"',
    ),
    "ip_accounts": Report("Account and IP pairs", ("actor", "ip"), ("Account or actor", "IP address")),
    "delivery": Report(
        "Message trace results", ("delivery",), ("Recorded delivery status",), 'source = "Exchange trace"'
    ),
    "outcomes": Report(
        "Sign-in outcomes", ("source", "result"), ("Sign-in log", "Outcome"), 'source contains "sign-ins"'
    ),
    "devices": Report("Devices", ("device",), ("Device ID",)),
    "sessions": Report("Sessions", ("session",), ("Session ID",)),
    "locations": Report("Sign-in locations", ("location",), ("Location",)),
    "applications": Report("Applications", ("application",), ("Application",)),
    "clients": Report("Client operating systems", ("os",), ("Operating system",)),
    "methods": Report("Authentication steps", ("authmethod", "result"), ("Method", "Recorded outcome")),
    "policies": Report("Conditional Access", ("conditionalaccess",), ("Recorded result",)),
    "rules": Report("Inbox rule changes", ("operation", "rule"), ("Operation", "Rule name or identity")),
    "access": Report("Mailbox access types", ("access",), ("Access type",)),
    "folders": Report("Accessed folders", ("folder",), ("Folder",)),
    "items": Report("Accessed mail items", ("mailitem",), ("Message or item identifier",)),
    "consent": Report(
        "Application and consent changes",
        ("operation", "result"),
        ("Directory operation", "Result"),
        'source = "Entra directory audit" AND (operation contains "consent" OR operation contains "application" OR operation contains "service principal" OR operation contains "app role" OR operation contains "permission grant")',
    ),
    "marks": Report(
        "Analyst-marked events",
        ("verdict", "source", "operation"),
        ("Verdict", "Source", "Operation"),
        "marked = true",
    ),
}

SIMPLE = {
    "source": SOURCE,
    "operation": "e.op",
    "actor": "e.actor",
    "result": "e.result",
    "file": "l.name",
    "verdict": store.VERDICT,
}


# Indicator categories can overlap.
INDICATORS = {
    "Inbox rules created, modified or removed": ("operation", "contains", ("InboxRule",)),
    "Mailbox and transport configuration": (
        "operation",
        "=",
        ("Set-Mailbox", "Set-MailboxAutoReplyConfiguration", "New-TransportRule", "Set-TransportRule"),
    ),
    "Delegation and send permissions": (
        "operation",
        "contains",
        ("MailboxPermission", "RecipientPermission", "FolderPermission"),
    ),
    "Messages sent": ("operation", "=", ("Send", "SendAs", "SendOnBehalf")),
    "Mailbox items accessed": ("operation", "=", ("MailItemsAccessed",)),
    "Attachments accessed": ("operation", "=", ("AttachmentAccess",)),
    "Messages deleted or moved to Deleted Items": (
        "operation",
        "=",
        ("HardDelete", "SoftDelete", "MoveToDeletedItems"),
    ),
    "Device registration or configuration": ("operation", "contains", ("device",)),
    "Password, credential or authentication changes": (
        "operation",
        "contains",
        ("password", "security info", "authentication method", "credential", "refreshTokenValidFrom"),
    ),
    "Account enabled or disabled": ("operation", "=", ("Enable account", "Disable account")),
    "Application consent or permission grants": (
        "operation",
        "contains",
        ("consent", "service principal", "app role", "permission grant"),
    ),
    "Failures and interrupted operations": (
        "result",
        "=",
        ("Failure", "Failed", "ClientError", "Interrupted"),
    ),
}


def indicator_expression(label):
    field, comparison, values = INDICATORS[label]
    return " OR ".join(f"{field} {comparison} {quote(v)}" for v in values)


def indicators(db, log, display_filter):
    clause, args = store.where(log, display_filter)
    sql = (
        """SELECT e.op,e.result,count(*),min(nullif(e.time,'')),max(nullif(e.time,''))
           FROM events e JOIN logs l ON l.id=e.log"""
        + clause
        + " GROUP BY e.op COLLATE NOCASE,e.result COLLATE NOCASE"
    )
    result = {label: [0, None, None] for label in INDICATORS}
    for op, res, count, first, last in db.execute(sql, args):
        for name, (field, test, values) in INDICATORS.items():
            value = (op if field == "operation" else res).lower()
            if any((s.lower() in value if test == "contains" else s.lower() == value) for s in values):
                group = result[name]
                group[0] += count
                if first:
                    group[1] = min(first, group[1] or first)
                if last:
                    group[2] = max(last, group[2] or last)
    return sorted([(name, *stats) for name, stats in result.items()], key=lambda r: (-r[1], r[0]))


def eligible(db, field, log):
    """Find logs containing this field."""
    candidates = []
    for row in db.execute("SELECT id,headers FROM logs"):
        if log and row["id"] != log:
            continue
        if supports(row["headers"], field):
            candidates.append((row["id"], row["headers"]))
    return candidates


def calculate(db, key, log, display_filter, interval=3600):
    if key == "indicators":
        return indicators(db, log, display_filter)
    if (
        key in ("files", "sources")
        and not display_filter
        and not db.execute("SELECT 1 FROM logs WHERE kind='Timeline' LIMIT 1").fetchone()
    ):
        # Labels can span exports.
        label = "l.name" if key == "files" else "source_label(l.kind)"
        rows = db.execute(
            f"""SELECT {label},sum(l.total),
                    min((SELECT min(time) FROM events WHERE log=l.id AND time>'')),
                    max((SELECT max(time) FROM events WHERE log=l.id AND time>''))
                FROM logs l WHERE l.total>0 AND (?=0 OR l.id=?)
                GROUP BY {label} COLLATE NOCASE ORDER BY sum(l.total) DESC,1""",
            (log or 0, log or 0),
        )
        return [tuple(row) for row in rows]
    report = REPORTS[key]
    clause, args = store.where(log, combine(display_filter, report.subset))
    joins = " FROM events e JOIN logs l ON l.id=e.log"
    exprs = []
    extra = []
    for index, field in enumerate(report.fields):
        alias = f"d{index}"
        if field == "time":
            if interval not in (60, 900, 3600, 86400):
                raise ValueError("Invalid interval")
            # Avoid midnight rounding errors.
            epoch = "unixepoch(substr(e.time,1,19))"
            expr = f"{epoch}-(({epoch}%{interval}+{interval})%{interval})"
        elif field in SIMPLE:
            expr = SIMPLE[field]
        elif field in ("ip", "mailbox"):
            table, col = ("ips", "ip") if field == "ip" else ("identities", "value")
            joins += f" JOIN {table} {alias} ON {alias}.event=e.id"
            expr = f"{alias}.{col}"
        else:
            candidates = eligible(db, field, log)
            if not candidates:
                return []
            clause += " AND e.log IN (" + ",".join("?" for _ in candidates) + ")"
            args.extend(k for k, _ in candidates)
            # Read unambiguous scalar columns directly.
            branches = []
            for k, headers in candidates:
                h = columns(headers)
                indices = [i for name in SLOTS[field][0] for i in h.get(name.casefold(), ())]
                if (
                    "auditdata" not in h
                    and field not in ("recipient", "target", "delivery")
                    and len(indices) == 1
                ):
                    branches.append(
                        f"WHEN {k} THEN json_array(trim(json_extract(e.raw,'$[{indices[0]}]')))"
                    )
            vals = "event_values(l.headers,e.raw,?)"
            if branches:
                vals = "CASE e.log " + " ".join(branches) + " ELSE " + vals + " END"
            joins += f" JOIN json_each({vals}) {alias}"
            extra.append(field)
            clause += f" AND {alias}.value IS NOT NULL AND {alias}.value!=''"
            expr = f"{alias}.value"
        exprs.append(expr)
        if field in SIMPLE or field == "time":
            clause += f" AND {expr} IS NOT NULL AND {expr}!=''"
    group = ",".join(
        expr + (" COLLATE BQL_TEXT" if field in SLOTS else " COLLATE NOCASE")
        for field, expr in zip(report.fields, exprs)
    )
    sql = (
        "SELECT "
        + ",".join(exprs)
        + ",count(*) AS frequency,min(nullif(e.time,'')),max(nullif(e.time,''))"
        + joins
        + clause
        + " GROUP BY "
        + group
        + " ORDER BY "
        + ("1" if key == "activity" else "frequency DESC,1")
    )
    return [tuple(row) for row in db.execute(sql, extra + args)]


def utc_text(epoch):
    return datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def drilldown(key, values, interval):
    if key == "indicators":
        return indicator_expression(values[0])
    report = REPORTS[key]
    if key == "activity":
        return f"time >= {quote(utc_text(values[0]))} AND time < {quote(utc_text(values[0]+interval))}"
    filters = [f"{field} = {quote(value)}" for field, value in zip(report.fields, values)]
    return combine(report.subset, " AND ".join(filters))


def quality(db):
    result = []
    for log in db.execute("SELECT id,name,sheet,kind,total,issues,warning FROM logs ORDER BY id"):
        linked = missing = None
        if log["kind"].endswith("auth details"):
            linked = db.execute(
                """SELECT count(*) FROM events e WHERE e.log=? AND e.request!='' AND EXISTS
                (SELECT 1 FROM events p JOIN logs l ON p.log=l.id WHERE p.request!='' AND p.request=e.request AND l.kind=?)""",
                (log["id"], log["kind"].replace("auth details", "sign-ins")),
            ).fetchone()[0]
            missing = log["total"] - linked
        result.append((*tuple(log), linked, missing))
    return result
