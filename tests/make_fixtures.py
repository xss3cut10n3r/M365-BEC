"""Generate synthetic investigation fixtures."""

import csv
import json
from pathlib import Path
import random
import sys

from openpyxl import Workbook


def write_csv(path, rows, encoding="utf-8-sig", delimiter=","):
    with path.open("w", newline="", encoding=encoding) as out:
        csv.writer(out, delimiter=delimiter).writerows(rows)


def audit(i, op, user, ip, extra=None):
    data = {
        "CreationTime": f"2026-01-02T{10 + i // 60 % 12:02d}:{i % 60:02d}:00",
        "Id": f"00000000-0000-0000-0000-{i:012d}",
        "Operation": op,
        "OrganizationId": "11111111-1111-1111-1111-111111111111",
        "RecordType": 2,
        "ResultStatus": "Succeeded" if i % 7 else "Failed",
        "UserId": user,
        "Workload": "Exchange",
        "ClientIP": ip,
        "ClientIPAddress": ip.split(":")[0] if ip.count(":") == 1 else ip,
        "MailboxOwnerUPN": user,
        "AppAccessContext": {"AADSessionId": f"session-{i % 5}"},
    }
    data.update(extra or {})
    return data


def small(root):
    """Mixed sources for parity checks."""
    rows = [["CreationDate", "UserIds", "Operations", "AuditData"]]
    users = ["analyst@example.org", "cfo@example.org", "Zoë.Müller@example.org"]
    ips = ["203.0.113.7", "198.51.100.23:51844", "[2001:db8::7]:443", "2001:DB8:0:0:0:0:0:9"]
    for i in range(60):
        op = ["New-InboxRule", "MailItemsAccessed", "Send", "Set-Mailbox", "HardDelete", "UserLoggedIn"][i % 6]
        extra = {}
        if op == "New-InboxRule":
            extra["Parameters"] = [
                {"Name": "Name", "Value": f"Invoice review {i}"},
                {"Name": "MoveToFolder", "Value": "RSS Feeds"},
            ]
        elif op == "MailItemsAccessed":
            extra["OperationProperties"] = [{"Name": "MailAccessType", "Value": "Bind" if i % 4 else "Sync"}]
            extra["Folders"] = [
                {
                    "Path": "\\Inbox",
                    "FolderItems": [{"InternetMessageId": f"<m{i}@example.org>"}, {"Id": f"item-{i}"}],
                }
            ]
        elif op == "Send":
            extra["Item"] = {
                "Subject": f"Wire details {i}" if i % 12 else "=cmd|' /C calc'!A0",
                "InternetMessageId": f"<s{i}@example.org>",
                "Recipients": [{"Address": "vendor@example.net"}, "second@example.net"],
            }
        data = audit(i, op, users[i % 3], ips[i % 4], extra)
        cell = json.dumps(data, ensure_ascii=bool(i % 2))
        if i == 17:
            cell = "{broken"
        if i == 23:
            cell = json.dumps(data, indent=2)
        rows.append([data["CreationTime"], data["UserId"], op, cell])
    write_csv(root / "MailboxAudit.csv", rows)

    head = [
        "Date (UTC)", "Request ID", "Correlation ID", "User ID", "User", "Username", "User type",
        "Application", "Application ID", "Resource", "IP address", "Location", "Status",
        "Sign-in error code", "Failure reason", "Client app", "Device ID", "Browser", "Operating System",
        "Session ID", "Conditional Access", "Incoming token type", "Incoming token type",
    ]
    rows = [head]
    for i in range(40):
        rows.append([
            f"2026-01-02T{9 + i // 20:02d}:{i % 60:02d}:{i % 50:02d}{'Z' if i % 3 else '+01:00'}",
            f"REQ-{i:04d}", f"corr-{i % 9}", f"uid-{i % 3}", "User " + str(i % 3), users[i % 3], "Member",
            ["Office 365 Exchange Online", "Azure Portal", "OfficeHome"][i % 3], f"app-{i % 3}",
            "Microsoft Graph", ["203.0.113.7", "192.0.2.44", "2001:db8::9"][i % 3],
            ["London, GB", "Lagos, NG", "Paris, FR"][i % 3], "Success" if i % 4 else "Failure",
            "0" if i % 4 else "50126", "" if i % 4 else "Invalid username or password", "Browser",
            f"device-{i % 4}" if i % 5 else "", "Edge 131", ["Windows10", "MacOs", "iOS"][i % 3],
            f"session-{i % 5}", ["Success", "Not Applied", "Failure"][i % 3], "None" if i % 2 else "", "Primary Refresh Token",
        ])
    rows.append(["not a date", "REQ-BAD", "", "", "", "ghost@example.org"])
    write_csv(root / "InteractiveSignIns_2026-01-02.csv", rows)

    rows = [["Date", "Request ID", "Authentication method", "Authentication method detail", "Succeeded", "Result detail", "Requirement"]]
    for i in range(0, 50, 2):
        rows.append([f"2026-01-02T09:{i % 60:02d}:01Z", f"req-{i:04d}", ["Password", "Mobile app notification"][i % 4 // 2], "", ["true", "false"][i % 3 == 0], "detail", "Primary authentication"])
    write_csv(root / "InteractiveSignIns_AuthDetails_2026-01-02.csv", rows, encoding="utf-16")

    rows = [["Date (UTC)", "CorrelationId", "Service", "Category", "Activity", "Result", "ResultReason", "ActorType", "ActorDisplayName", "ActorObjectId", "ActorUserPrincipalName", "IPAddress", "ActorServicePrincipalId", "Target1Type", "Target1DisplayName", "Target1UserPrincipalName", "Target1ObjectId", "Target1ModifiedProperty1Name", "Target1ModifiedProperty1OldValue", "Target1ModifiedProperty1NewValue", "AdditionalDetail1Key", "AdditionalDetail1Value"]]
    acts = ["Consent to application", "Add service principal", "Update user", "Register device", "Change user password", "Disable account", "Add app role assignment to service principal"]
    for i in range(30):
        rows.append([f"2026-01-0{1 + i % 3}T08:{i:02d}:00.{i:03d}Z", f"corr-{i % 9}", "Core Directory", "ApplicationManagement", acts[i % 7], "success" if i % 6 else "failure", "", "User", "Admin", "obj-1", "admin@example.org", "192.0.2.44", "", "User", f"Target {i}", users[i % 3], f"obj-t{i}", "ConsentAction.Permissions", "[]", json.dumps([{"Scope": "Mail.Read", "n": i}]), "User-Agent", "Mozilla/5.0"])
    write_csv(root / "AuditLogs_2026-01-03.csv", rows, delimiter=";")

    rows = [["origin_timestamp", "sender_address", "recipient_status", "message_subject", "total_bytes", "message_id", "network_message_id", "original_client_ip", "directionality"]]
    for i in range(25):
        rows.append([f"2026-01-02T11:{i:02d}:59.9999990Z", users[i % 3], f"vendor@example.net##Receive, {'Deliver' if i % 5 else 'Fail'};b{i % 2}@example.org##Receive, Deliver", f"Trace {i}\nline two", str(1000 + i), f"<t{i}@example.org>", f"net-{i}", "198.51.100.23", "Originating"])
    write_csv(root / "MessageTrace.csv", rows)

    book = Workbook()
    sheet = book.active
    sheet.title = "Sign-ins"
    sheet.append(["Date (UTC)", "Request ID", "Username", "IP address", "Status", "Count", "Ratio", "Flag"])
    import datetime as dt
    for i in range(12):
        sheet.append([dt.datetime(2026, 1, 4, 7, i, 30, 250000 * (i % 4)), f"x-{i}", users[i % 3], "192.0.2.44", "Success", i * 1000, i / 8, bool(i % 2)])
    sheet.append(["2026-01-04T08:00:00Z", "x-f", "=HYPERLINK(\"http://x\")", "bad-ip", None, 12345678901234567890, 1e21, None])
    other = book.create_sheet("Notes")
    other.append(["Actor", "Operation", "Date"])
    other.append(["analyst@example.org", "=1+1", dt.date(2026, 1, 5)])
    other.append(["cfo@example.org", "Reviewed", dt.time(4, 5, 6)])
    book.create_sheet("Empty report").append(["Actor"])
    book.save(root / "recurrence-review.xlsx")

    graph = {
        "@odata.context": "https://graph.microsoft.com/v1.0/$metadata#auditLogs/signIns",
        "value": [
            {
                "id": f"g-{i}",
                "createdDateTime": f"2026-01-05T06:{i:02d}:00Z",
                "userPrincipalName": users[i % 3],
                "appDisplayName": "Graph Explorer",
                "ipAddress": "203.0.113.7",
                "riskLevel": None,
                "isInteractive": bool(i % 2),
                "status": {"errorCode": 0 if i % 3 else 50126, "failureReason": None},
                "location": {"city": "London", "geoCoordinates": {"latitude": 51.5, "longitude": -0.12}},
                "appliedConditionalAccessPolicies": [{"displayName": "MFA", "result": "success"}, {"displayName": "Block legacy", "result": "notApplied"}],
            }
            for i in range(20)
        ],
    }
    (root / "graph-signins.json").write_text(json.dumps(graph, indent=1), encoding="utf-8")
    events = [
        {"CreationTime": "2026-01-05T09:00:00", "Operation": "Send", "UserId": "cfo@example.org", "ClientIP": "198.51.100.23", "ResultStatus": "Succeeded", "Item": {"Subject": "Zahlung", "Sizes": [1, 2.5, 1e3]}, "Big": 12345678901234567890, "Flag": True, "Nothing": None},
        {"Operation": "HardDelete", "CreationTime": "2026-01-05T09:05:00", "UserId": "cfo@example.org", "Extra": {"deep": [{"k": "v"}]}},
    ]
    (root / "audit-array.json").write_text(json.dumps(events), encoding="utf-8")
    (root / "single-event.json").write_text(json.dumps(events[0], ensure_ascii=False), encoding="utf-8")
    (root / "empty-array.json").write_text("[]", encoding="utf-8")
    (root / "malformed.json").write_text('[{"Operation": "Send"}, {"Operation": ', encoding="utf-8")
    (root / "ambiguous.json").write_text('{"value": [{"a": 1}], "Operation": "Send"}', encoding="utf-8")
    (root / "malformed.csv").write_text('Operation,AuditData\nOK,{}\nBroken,"unterminated', encoding="utf-8")


def large(root, count=100000):
    """Equal CSV and JSON loads."""
    rng = random.Random(7)
    ops = ["MailItemsAccessed", "Send", "New-InboxRule", "UserLoggedIn", "FileAccessed", "SoftDelete"]
    events = []
    with (root / "large-audit.csv").open("w", newline="", encoding="utf-8-sig") as out:
        writer = csv.writer(out)
        writer.writerow(["CreationDate", "UserIds", "Operations", "AuditData"])
        for i in range(count):
            user = f"user{rng.randrange(400)}@example.org"
            ip = f"198.51.{rng.randrange(100)}.{rng.randrange(1, 255)}"
            data = audit(i, ops[rng.randrange(6)], user, ip)
            data["CreationTime"] = f"2026-02-{1 + i * 27 // count:02d}T{i // 3600 % 24:02d}:{i // 60 % 60:02d}:{i % 60:02d}"
            data["Folders"] = [{"Path": "\\Inbox", "FolderItems": [{"InternetMessageId": f"<{i}@example.org>"}]}]
            writer.writerow([data["CreationTime"], user, data["Operation"], json.dumps(data, separators=(",", ":"))])
            events.append(data)
    with (root / "large-audit.json").open("w", encoding="utf-8") as out:
        json.dump(events, out, separators=(",", ":"))


if __name__ == "__main__":
    target = Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    small(target)
    if len(sys.argv) > 2:
        large(target, int(sys.argv[2]))
