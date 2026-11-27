"""Decode shared evidence fields."""

from functools import lru_cache
import json
import re

# Column and AuditData aliases.
SLOTS = {
    "device": (("Device ID",), ("DeviceId",)),
    "session": (("Session ID",), ("SessionId", "AADSessionId")),
    "correlation": (("Correlation ID", "CorrelationId"), ("CorrelationId",)),
    "application": (("Application",), ("ApplicationDisplayName",)),
    "appid": (("Application ID",), ("AppId", "ClientAppId")),
    "location": (("Location",), ()),
    "os": (("Operating System",), ()),
    "client": (("Client app",), ("ClientInfoString",)),
    "authmethod": (("Authentication method",), ()),
    "conditionalaccess": (("Conditional Access",), ()),
    "serviceprincipal": (("Service principal ID", "ActorServicePrincipalId"), ()),
    "workload": (("Service",), ("Workload",)),
    "messageid": (("message_id", "MessageId", "Message ID"), ()),
    "recipient": (("recipient_address", "RecipientAddress", "Recipient address", "recipient_status"), ()),
    "delivery": (("recipient_status", "Status"), ()),
    "subject": (("message_subject", "Subject"), ()),
    "rule": ((), ()),
    "mailitem": ((), ()),
    "access": ((), ()),
    "folder": ((), ()),
    "target": ((), ()),
}


@lru_cache(maxsize=128)
def columns(headers):
    positions = {}
    for i, name in enumerate(json.loads(headers)):
        positions.setdefault(" ".join(name.casefold().split()), []).append(i)
    return positions


def decode(value):
    if isinstance(value, str) and value.lstrip().startswith(("{", "[")):
        try:
            return json.loads(value)
        except (ValueError, RecursionError):
            pass
    return value


def objects(value):
    return (item for item in value if isinstance(item, dict)) if isinstance(value, list) else ()


class EventFields:
    def __init__(self, headers, raw):
        self.columns = columns(headers)
        self.row = json.loads(raw)
        self._audit = None

    def outer(self, name):
        return [self.row[i] for i in self.columns.get(name.casefold(), ()) if i < len(self.row)]

    @property
    def audit(self):
        if self._audit is None:
            value = decode(next(iter(self.outer("AuditData")), ""))
            self._audit = value if isinstance(value, dict) else {}
        return self._audit

    def values(self, field):
        outer, inner = SLOTS[field]
        values = [value for name in outer for value in self.outer(name)]
        audit = self.audit
        values.extend(audit.get(name) for name in inner)
        item = audit.get("Item")
        item = item if isinstance(item, dict) else {}
        if field == "session":
            ctx = audit.get("AppAccessContext")
            if isinstance(ctx, dict):
                values.append(ctx.get("AADSessionId"))
        elif field == "recipient":
            values = [part.split("##")[0].strip() for v in values for part in str(v).split(";")]
            recipients = item.get("Recipients", [])
            if isinstance(recipients, list):
                for rec in recipients:
                    if isinstance(rec, dict):
                        values.append(
                            rec.get("Address") or rec.get("EmailAddress") or rec.get("SmtpAddress")
                        )
                    elif isinstance(rec, str):
                        values.append(rec)
        elif field == "delivery":
            values = list(self.outer("Status"))
            for cell in self.outer("recipient_status"):
                for entry in cell.split(";"):
                    address, separator, status = entry.partition("##")
                    if separator:
                        values.append(status)
        elif field == "subject":
            values.append(item.get("Subject"))
        elif field == "messageid":
            values.append(item.get("InternetMessageId"))
        elif field in ("folder", "mailitem"):
            for folder in objects(audit.get("Folders")):
                if field == "folder":
                    values.append(folder.get("Path"))
                else:
                    for entry in objects(folder.get("FolderItems")):
                        values.append(
                            entry.get("InternetMessageId") or entry.get("ImmutableId") or entry.get("Id")
                        )
        elif field == "rule" and "rule" in str(audit.get("Operation", "")).casefold():
            for param in objects(audit.get("Parameters")):
                if param.get("Name") in ("Name", "Identity"):
                    values.append(param.get("Value"))
        elif field == "access":
            for prop in objects(audit.get("OperationProperties")):
                if prop.get("Name") == "MailAccessType":
                    values.append(prop.get("Value"))
        elif field == "target":
            for name in self.columns:
                if re.fullmatch(r"target\d+(userprincipalname|displayname|objectid)", name):
                    values.extend(self.outer(name))
        unique = {}
        for val in values:
            if isinstance(val, str) and val.strip():
                val = val.strip()
                unique.setdefault(val.casefold(), val)
        return list(unique.values())


def sql_values(headers, raw, field):
    return json.dumps(EventFields(headers, raw).values(field), ensure_ascii=False)


@lru_cache(maxsize=4096)
def supports(headers, field):
    h = columns(headers)
    return (
        "auditdata" in h
        or any(name.casefold() in h for name in SLOTS[field][0])
        or field == "target"
        and any(key.startswith("target") for key in h)
    )


def sql_match(headers, raw, field, op, value):
    if not supports(headers, field):
        return op == "!="
    needle = value.casefold()
    values = EventFields(headers, raw).values(field)
    if op == "contains":
        return any(needle in v.casefold() for v in values)
    equal = any(v.casefold() == needle for v in values)
    return not equal if op == "!=" else equal


def escape_pointer(key):
    return str(key).replace("~", "~0").replace("/", "~1")


@lru_cache(maxsize=256)
def pointer_parts(pointer):
    return tuple(p.replace("~1", "/").replace("~0", "~") for p in pointer[1:].split("/"))


def pointer_values(headers, raw, pointer, *, presence=False):
    """Resolve nested and duplicate fields."""
    head, *parts = pointer_parts(pointer)
    occurrence = re.match(r"^@([1-9]\d{0,5}):(.*)$", head, re.DOTALL)
    if occurrence:
        head = occurrence[2]
    positions = columns(headers).get(" ".join(head.casefold().split()), ())
    if occurrence:
        ordinal = int(occurrence[1]) - 1
        positions = positions[ordinal : ordinal + 1]
    if not positions:
        return
    row = json.loads(raw)
    stack = [(row[i], 0) for i in reversed(positions) if i < len(row)]
    while stack:
        actual, depth = stack.pop()
        if depth == len(parts):
            if presence:
                yield ""
                return
            if isinstance(actual, (list, dict)):
                continue
            yield actual if isinstance(actual, str) else json.dumps(actual)
            continue
        obj = decode(actual)
        key = parts[depth]
        if isinstance(obj, dict) and key in obj:
            stack.append((obj[key], depth + 1))
        elif isinstance(obj, list):
            if key == "*":
                stack.extend((item, depth + 1) for item in reversed(obj))
            elif key.isdecimal() and len(key) < 10 and int(key) < len(obj):
                stack.append((obj[int(key)], depth + 1))


def match_path(headers, raw, pointer, op, value):
    if op == "exists":
        return next(pointer_values(headers, raw, pointer, presence=True), None) is not None
    target = value.casefold()
    for actual in pointer_values(headers, raw, pointer):
        actual = actual.casefold()
        if target in actual if op == "contains" else target == actual:
            return op != "!="
    return op == "!="


def field_display(headers, raw, pointer):
    return " · ".join(dict.fromkeys(pointer_values(headers, raw, pointer)))
