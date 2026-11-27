"""Compile parameterized display filters."""

import datetime as dt
import ipaddress
import json
import re
from event_fields import SLOTS


def utc(value):
    """Normalize timestamps for chronological sorting."""
    if value is None or value == "":
        return ""
    if not isinstance(value, dt.datetime):
        try:
            value = dt.datetime.fromisoformat(str(value).strip().replace("Z", "+00:00"))
        except ValueError:
            return ""
    if value.tzinfo is None:
        value = value.replace(tzinfo=dt.timezone.utc)
    return value.astimezone(dt.timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def address(value):
    value = str(value or "").strip()
    match = re.fullmatch(r"\[([^\]]+)\](?::\d+)?|(\d+\.\d+\.\d+\.\d+):\d+", value)
    if match:
        value = match.group(1) or match.group(2)
    try:
        return str(ipaddress.ip_address(value))
    except ValueError:
        return ""


MARK_IP = "EXISTS (SELECT 1 FROM ips i JOIN marks m ON m.kind='ip' AND m.lo=i.ip WHERE i.event=e.id)"
MARK_TIME = "EXISTS (SELECT 1 FROM marks m WHERE m.kind='time' AND e.time>=m.lo AND e.time<m.hi)"
CASE_ID = "(SELECT number FROM timeline_ids WHERE event=e.id)"
# Rules use permanent event keys.
LATEST_RULE = """CASE WHEN EXISTS(SELECT 1 FROM verdict_rules) THEN (SELECT max(rule) FROM (
    SELECT max(id) AS rule FROM verdict_rules WHERE kind='event' AND lo=CAST(e.id AS TEXT)
    UNION ALL SELECT max(r.id) FROM ips i JOIN verdict_rules r ON r.kind='ip' AND r.lo=i.ip WHERE i.event=e.id
    UNION ALL SELECT max(id) FROM verdict_rules WHERE kind='time' AND lo<=e.time AND hi>e.time
)) END"""
LEGACY_REASON = f"CASE WHEN {MARK_IP} THEN CASE WHEN {MARK_TIME} THEN 'IP + Time range' ELSE 'IP' END WHEN {MARK_TIME} THEN 'Time range' ELSE '' END"
VERDICT = f"coalesce((SELECT verdict FROM verdict_rules WHERE id={LATEST_RULE}), CASE WHEN {MARK_IP} OR {MARK_TIME} THEN 'Bad' ELSE 'Unmarked' END)"
REASON = f"""coalesce((SELECT CASE WHEN verdict='Unmarked' THEN '' WHEN kind='event' THEN 'Event'
    WHEN kind='ip' THEN 'IP' ELSE 'Time range' END FROM verdict_rules WHERE id={LATEST_RULE}),{LEGACY_REASON})"""

# Timelines may mix providers.
SOURCE = """source_label(CASE WHEN l.kind='Timeline' THEN COALESCE(
    (SELECT NULLIF(trim(json_extract(e.raw,'$[' || h.key || ']')),'')
     FROM json_each(l.headers) h WHERE lower(trim(h.value))='source' LIMIT 1), l.kind)
    ELSE l.kind END)"""


ENTRA_EXPORTS = {
    f"{family} {report}".lower(): f"Entra · {family} {report}"
    for family in ("Interactive", "Non-interactive", "Application", "Managed identity", "Workload", "User")
    for report in ("sign-ins", "auth details")
}


def source_label(kind):
    """Display legacy Entra source names."""
    return ENTRA_EXPORTS.get(kind.casefold(), kind)


FIELDS = {
    "source": SOURCE,
    "file": "l.name",
    "sheet": "l.sheet",
    "path": "(SELECT path FROM files WHERE id=l.file)",
    "sha256": "(SELECT digest FROM files WHERE id=l.file)",
    "issue": "e.issue",
    "markreason": REASON,
    "verdict": VERDICT,
    "time": "e.time",
    "operation": "e.op",
    "result": "e.result",
    "request": "e.request",
    "actor": "e.actor",
    "raw": "e.raw",
}
TOKEN = re.compile(r'\s*("(?:\\.|[^"\\])*"|!=|>=|<=|==|[()=<>]|[^\s()=!<>"]+)')


class Query:
    def __init__(self, text):
        self.tokens = []
        self.args = []
        text = text.strip()
        pos = 0
        while pos < len(text):
            match = TOKEN.match(text, pos)
            if not match:
                raise ValueError(f"Invalid filter near character {pos + 1}")
            self.tokens.append(match.group(1))
            pos = match.end()
        self.index = 0
        self.sql = self.expression() if self.tokens else "1"
        if self.index != len(self.tokens):
            raise ValueError(f"Unexpected token {self.tokens[self.index]}")

    def take(self, token):
        if self.index < len(self.tokens) and self.tokens[self.index].lower() == token:
            self.index += 1
            return True
        return False

    def next(self):
        if self.index >= len(self.tokens):
            raise ValueError("Incomplete filter")
        self.index += 1
        return self.tokens[self.index - 1]

    def expression(self):
        parts = [self.conjunction()]
        while self.take("or"):
            parts.append(self.conjunction())
        return "(" + " OR ".join(parts) + ")"

    def conjunction(self):
        parts = [self.term()]
        while self.take("and"):
            parts.append(self.term())
        return "(" + " AND ".join(parts) + ")"

    def term(self):
        if self.take("not"):
            return "(NOT " + self.term() + ")"
        if self.take("("):
            result = self.expression()
            if not self.take(")"):
                raise ValueError("Missing closing parenthesis")
            return result
        field = self.next().lower()
        if field in ("field", "exists"):
            literal = self.next()
            if not literal.startswith('"'):
                raise ValueError('Use field "/Header/nested/key" = "value"')
            pointer = json.loads(literal)
            if not pointer.startswith("/") or len(pointer) > 4096:
                raise ValueError("Field paths start with / and use up to 4096 characters")
            if field == "exists":
                self.args.append(pointer)
                return "match_path(l.headers,e.raw,?,'exists','')"
        op = self.next().lower()
        if op not in {"=", "==", "!=", "<", "<=", ">", ">=", "contains"}:
            raise ValueError("Expected comparison or contains")
        value = self.next()
        if value.startswith('"'):
            value = json.loads(value)
        if field == "field":
            if op not in ("=", "==", "!=", "contains"):
                raise ValueError("Use equality or contains with a source field")
            self.args.extend([pointer, op, value])
            return "match_path(l.headers,e.raw,?,?,?)"
        if field == "marked":
            if value.lower() not in ("true", "false") or op not in ("=", "==", "!="):
                raise ValueError("Use marked = true or marked = false")
            invert = (value.lower() == "false") != (op == "!=")
            return f"({VERDICT} {'=' if invert else '!='} 'Unmarked')"
        if field == "time":
            value = utc(value)
            if not value or op == "contains":
                raise ValueError("Compare time with an ISO date or timestamp")
        if field in SLOTS:
            if op not in ("=", "==", "!=", "contains"):
                raise ValueError(f"Use equality or contains with {field}")
            self.args.extend([field, op, value])
            return "event_match(l.headers,e.raw,?,?,?)"
        if field in ("row", "id"):
            if op == "contains" or not value.isdecimal() or len(value) > 19 or int(value) > 2**63 - 1:
                raise ValueError(f"Compare {field} with an integer in range")
            self.args.append(int(value))
            if field == "id":
                return f"e.id IN (SELECT event FROM timeline_ids WHERE number {op} ?)"
            return f"e.rownum {op} ?"
        if field in ("mailbox", "ip"):
            if op not in ("=", "==", "!=", "contains"):
                raise ValueError(f"Use equality or contains with {field}")
            if field == "ip" and op != "contains":
                value = address(value)
                if not value:
                    raise ValueError("Invalid IP address")
            table, col = ("ips", "ip") if field == "ip" else ("identities", "value")
            condition = (
                f"{col} = ? COLLATE NOCASE" if op != "contains" else f"instr(lower({col}),lower(?))>0"
            )
            self.args.append(value)
            return (
                "NOT " if op == "!=" else ""
            ) + f"EXISTS (SELECT 1 FROM {table} WHERE event=e.id AND {condition})"
        if field not in FIELDS:
            raise ValueError(f"Unknown field: {field}")
        if field == "source":
            if op in ("=", "==", "!="):
                value = source_label(value)

            def compare(expr):
                return (
                    f"instr(lower({expr}),lower(?))>0"
                    if op == "contains"
                    else f"{expr} {op} ? COLLATE NOCASE"
                )

            # Native sources use log indexes.
            self.args.extend([value, value])
            return (
                "(e.log IN (SELECT id FROM logs WHERE kind!='Timeline' AND "
                + compare("source_label(kind)")
                + ") OR (e.log IN (SELECT id FROM logs WHERE kind='Timeline') AND "
                + compare(SOURCE)
                + "))"
            )
        self.args.append(value)
        col = FIELDS[field]
        return f"instr(lower({col}),lower(?))>0" if op == "contains" else f"{col} {op} ? COLLATE NOCASE"
