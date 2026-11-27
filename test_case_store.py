import csv
import json
from pathlib import Path
import tempfile
import unittest

from openpyxl import Workbook, load_workbook

import case_store as store
from bql import Query


class CaseTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.db = store.connect(self.root / "test.bec")
        self.addCleanup(self.db.close)

    def write_csv(self, name, rows):
        path = self.root / name
        with path.open("w", newline="", encoding="utf-8-sig") as stream:
            csv.writer(stream).writerows(rows)
        return path

    def test_original_and_duplicate_columns_survive_reimport(self):
        path = self.write_csv(
            "InteractiveSignIns.csv",
            [
                ["Request ID", "Username", "Incoming token type", "Incoming token type", "IP address"],
                ["r1", "user@example.org", "first\nline", "second", "[2001:db8::1]:443"],
            ],
        )
        store.import_file(self.db, path)
        store.import_file(self.db, path)
        rows = list(store.query_rows(self.db, 0, 'ip = "2001:db8::1" AND mailbox = "USER@example.org"'))
        self.assertEqual([r["rownum"] for r in rows], [2])
        saved = self.db.execute("SELECT raw FROM events").fetchone()[0]
        self.assertEqual(json.loads(saved)[2:4], ["first\nline", "second"])
        out = self.root / "copy.csv"
        store.extract(self.db, 1, out)
        self.assertEqual(out.read_bytes(), path.read_bytes())
        with self.assertRaises(ValueError):
            store.extract(self.db, 1, path)

    def test_malformed_input_rolls_back_and_bad_payload_stays_visible(self):
        path = self.root / "bad.csv"
        path.write_text('Operation,AuditData\nOK,{}\nBroken,"unterminated')
        with self.assertRaises(csv.Error):
            store.import_file(self.db, path)
        self.assertEqual(self.db.execute("SELECT count(*) FROM files").fetchone()[0], 0)
        good = self.write_csv("audit.csv", [["Operation", "AuditData"], ["Test", "{"]])
        store.import_file(self.db, good)
        self.assertEqual(self.db.execute("SELECT issues FROM logs").fetchone()[0], 1)
        self.assertEqual(self.db.execute("SELECT count(*) FROM events").fetchone()[0], 1)

    def test_header_only_workload_files_keep_their_identities(self):
        for name in ("ApplicationSignIns.csv", "MSISignIns.csv"):
            path = self.write_csv(name, [["Request ID", "Service principal ID"]])
            store.import_file(self.db, path)
        logs = list(self.db.execute("SELECT kind,total FROM logs ORDER BY id"))
        self.assertEqual(
            [tuple(r) for r in logs], [("Application sign-ins", 0), ("Managed identity sign-ins", 0)]
        )

    def test_overlapping_marks_and_query_precedence(self):
        p = self.write_csv(
            "events.csv",
            [
                ["Date (UTC)", "Username", "IP address", "Operation"],
                ["2026-01-02T12:00:00Z", "a@example.org", "203.0.113.7", "Send"],
                ["2026-01-03T12:00:00Z", "b@example.org", "192.0.2.8", "Read"],
            ],
        )
        store.import_file(self.db, p)
        with self.db:
            self.db.execute("INSERT INTO marks(kind,lo,hi) VALUES('ip','203.0.113.7','')")
            self.db.execute(
                "INSERT INTO marks(kind,lo,hi) VALUES('time',?,?)",
                (store.utc("2026-01-02"), store.utc("2026-01-03")),
            )
        rows = list(
            store.query_rows(self.db, 0, "marked = true AND (operation = Send OR operation = Read)")
        )
        self.assertEqual([store.mark_label(r) for r in rows], ["IP + Time range"])
        with self.db:
            self.db.execute("DELETE FROM marks WHERE kind='time'")
        self.assertEqual(store.count_rows(self.db, 0, "NOT (marked = false)"), 1)
        with self.assertRaises(ValueError):
            Query("ip = broken OR")

    def test_workbook_sheets_and_literal_formula_export(self):
        book = Workbook()
        book.active.append(["Date (UTC)", "Actor", "Operation"])
        book.active.append(["2026-01-01T00:00:00Z", "user@example.org", "=1+1"])
        book.create_sheet("Empty report").append(["Actor"])
        path = self.root / "input.xlsx"
        book.save(path)
        store.import_file(self.db, path)
        self.assertEqual([r[0] for r in self.db.execute("SELECT total FROM logs ORDER BY id")], [1, 0])
        out = self.root / "out.xlsx"
        store.export(self.db, out, 0, "")
        exported = load_workbook(out, read_only=True)
        try:
            cell = list(exported.active.rows)[1][6]
            self.assertEqual((cell.value, cell.data_type), ("=1+1", "s"))
        finally:
            exported.close()
        table = self.root / "out.csv"
        store.export(self.db, table, 0, "")
        with table.open(encoding="utf-8-sig", newline="") as src:
            self.assertEqual(list(csv.reader(src))[1][6], "'=1+1")

    def test_mixed_sources_survive_export_and_reimport(self):
        for name, headers, row in [
            ("InteractiveSignIns.csv", ["Request ID", "Username"], ["r1", "user@example.org"]),
            ("audit.csv", ["Operation", "AuditData"], ["Read", "{}"]),
        ]:
            store.import_file(self.db, self.write_csv(name, [headers, row]))
        expected = ["Entra · Interactive sign-ins", "Purview audit"]
        self.assertEqual([r["source"] for r in store.query_rows(self.db, 0, "")], expected)
        for ext in ("csv", "xlsx"):
            path = self.root / ("mixed." + ext)
            store.export(self.db, path, 0, 'file != "mixed.csv"')
            store.import_file(self.db, path)
            log_id = self.db.execute("SELECT id FROM logs WHERE name=?", (path.name,)).fetchone()[0]
            rows = list(store.query_rows(self.db, log_id, ""))
            self.assertEqual([r["source"] for r in rows], expected)
            self.assertEqual({r["name"] for r in rows}, {path.name})
            for label in ("Interactive sign-ins", expected[0]):
                self.assertEqual(store.count_rows(self.db, log_id, f'source = "{label}"'), 1)


if __name__ == "__main__":
    unittest.main()
