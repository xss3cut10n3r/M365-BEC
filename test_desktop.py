import csv
from contextlib import closing
import json
import os
from pathlib import Path
import tempfile
import time
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import QPoint, QSettings, Qt, QTimer
from PySide6.QtGui import QContextMenuEvent, QTextCursor
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMenu, QStyle, QStyleOptionTab, QTabBar, QToolButton

from analyzer import Analyzer, APP
import case_store as store
from case_store import connect, import_file, count_rows, query_rows, export
from desktop_style import COLS, VERDICT_COLORS, style
from telemetry import REPORTS, calculate, combine, drilldown


class DesktopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        style(cls.app)

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "review.bec"
        self.db = connect(self.path)
        self.addCleanup(self.db.close)
        self.audit = {
            "Operation": "Send",
            "CreationTime": "2026-01-02T11:04:00",
            "UserId": "a@example.org",
            "ClientIP": "192.0.2.8:443",
            "ClientIPAddress": "192.0.2.8",
            "ResultStatus": "Success",
            "AppAccessContext": {"AADSessionId": "s1"},
            "Item": {
                "Subject": "Invoice",
                "Recipients": [
                    {"Address": "b@example.org"},
                    {"Address": "b@example.org"},
                    {"Address": "c@example.org"},
                ],
            },
            "Data/~": [{"x": False}, {"x": "needle"}],
        }
        self.write("audit.csv", [["Operation", "AuditData"], ["Send", json.dumps(self.audit)]])
        self.write(
            "InteractiveSignIns.csv",
            [
                [
                    "Request ID",
                    "Date (UTC)",
                    "Username",
                    "IP address",
                    "Session ID",
                    "Device ID",
                    "Location",
                    "Status",
                    "Incoming token type",
                    "Incoming token type",
                ],
                [
                    "r1",
                    "2026-01-02T11:01:00Z",
                    "a@example.org",
                    "192.0.2.8",
                    "s1",
                    "d1",
                    "London",
                    "Success",
                    "First",
                    "Second",
                ],
                [
                    "r2",
                    "2026-01-02T13:05:00Z",
                    "a@example.org",
                    "192.0.2.9",
                    "s2",
                    "d2",
                    "Paris",
                    "Failure",
                    "",
                    "",
                ],
            ],
        )
        self.write(
            "InteractiveSignIns_AuthDetails.csv",
            [
                ["Request ID", "Date", "Authentication method", "Succeeded"],
                ["r1", "2026-01-02T11:01:00Z", "Windows Hello", "true"],
            ],
        )

    def write(self, name, rows):
        p = self.path.parent / name
        with p.open("w", newline="", encoding="utf8") as out:
            csv.writer(out).writerows(rows)
        import_file(self.db, p)

    def wait(self, check, seconds=12):
        end = time.monotonic() + seconds
        while not check() and time.monotonic() < end:
            self.app.processEvents()
            time.sleep(0.015)
        self.app.processEvents()
        self.assertTrue(check())

    def test_all_statistics_drill_to_exact_filtered_counts(self):
        for scope, base in [(0, ""), (1, 'actor = "a@example.org"')]:
            for key, spec in REPORTS.items():
                with self.subTest(key=key, scope=scope):
                    rows = calculate(self.db, key, scope, base, 3600)
                    for r in rows:
                        expr = combine(base, drilldown(key, r, 3600))
                        self.assertEqual(count_rows(self.db, scope, expr), r[len(spec.fields)])
        self.assertEqual(len(calculate(self.db, "conversations", 0, "")), 2)
        self.assertEqual(count_rows(self.db, 0, 'session = "s1"'), 2)
        self.assertEqual(count_rows(self.db, 0, 'field "/AuditData/Data~1~0/*/x" = "false"'), 1)
        self.assertEqual(count_rows(self.db, 0, 'field "/Incoming token type" = "Second"'), 1)
        self.assertEqual(count_rows(self.db, 0, 'field "/@1:Incoming token type" = "Second"'), 0)
        self.assertEqual(count_rows(self.db, 0, 'field "/@2:Incoming token type" = "Second"'), 1)

    def test_exchange_trace_recipients_statuses_and_fractional_hour(self):
        self.write(
            "enhanced-trace.csv",
            [
                ["origin_timestamp", "message_id", "sender_address", "recipient_status", "message_subject"],
                [
                    "2026-01-02T11:59:59.999999Z",
                    "<m1@example.org>",
                    "sender@example.org",
                    "b@example.org##Receive, Deliver;c@example.org##Receive, Fail",
                    "Review",
                ],
                [
                    "2026-01-02T12:00:00Z",
                    "<m2@example.org>",
                    "sender@example.org",
                    "c@example.org##Receive, Deliver",
                    "Follow up",
                ],
            ],
        )
        scope = self.db.execute("SELECT id FROM logs WHERE kind='Exchange trace'").fetchone()[0]
        delivery = calculate(self.db, "delivery", scope, "")
        self.assertEqual([r[:2] for r in delivery], [("Receive, Deliver", 2), ("Receive, Fail", 1)])
        self.assertEqual(
            [r[:3] for r in calculate(self.db, "conversations", scope, "")],
            [("sender@example.org", "c@example.org", 2), ("sender@example.org", "b@example.org", 1)],
        )
        self.assertEqual([r[1] for r in calculate(self.db, "activity", scope, "")], [1, 1])
        for key in ("delivery", "conversations", "activity", "sources"):
            for r in calculate(self.db, key, scope, ""):
                self.assertEqual(
                    count_rows(self.db, scope, drilldown(key, r, 3600)), r[len(REPORTS[key].fields)]
                )

    def test_close_and_reopen_panes_and_log_tabs(self):
        settings = QSettings(str(self.path.parent / "panes.ini"), QSettings.Format.IniFormat)
        w = Analyzer(settings)
        self.addCleanup(w.close)
        w.show()
        w.load(self.path)
        self.wait(lambda: not w.loading)

        def button(name):
            return next(b for b in w.findChildren(QToolButton) if b.accessibleName() == name)

        for name, key in (("Close event details", "details"), ("Close source record", "source")):
            QTest.mouseClick(button(name), Qt.MouseButton.LeftButton)
            self.assertFalse(w.pane_actions[key].isChecked())
        self.assertTrue(w.inspector.isHidden())
        w.actions["find"].trigger()
        self.assertTrue(w.inspector.find.isVisible())
        self.assertTrue(w.inspector.widget(0).isHidden())
        w.pane_actions["details"].trigger()
        self.assertTrue(w.inspector.widget(0).isVisible())

        tabs = w.tabs
        QTest.mouseClick(tabs, Qt.MouseButton.LeftButton, pos=tabs.tabRect(1).center())
        self.wait(lambda: not w.loading)
        opt = QStyleOptionTab()
        tabs.initStyleOption(opt, 1)
        self.assertFalse(opt.state & QStyle.StateFlag.State_HasFocus)
        self.assertTrue(tabs.focusPolicy() & Qt.FocusPolicy.TabFocus)
        tabs.setFocus(Qt.FocusReason.TabFocusReason)
        QTest.keyClick(tabs, Qt.Key.Key_Right)
        self.assertEqual(w.active, 2)
        tabs.initStyleOption(opt, tabs.currentIndex())
        self.assertTrue(opt.state & QStyle.StateFlag.State_HasFocus)
        QTest.mouseClick(tabs, Qt.MouseButton.LeftButton, pos=tabs.tabRect(1).center())
        tabs.initStyleOption(opt, tabs.currentIndex())
        self.assertFalse(opt.state & QStyle.StateFlag.State_HasFocus)
        w.apply('operation = "Send"')
        self.wait(lambda: not w.loading)
        tabs.moveTab(tabs.currentIndex(), 0)
        self.wait(lambda: not w.loading)
        token = w.query_token
        other = tabs.tabData(2)
        QTest.mouseClick(tabs.tabButton(2, QTabBar.RightSide), Qt.LeftButton)
        self.assertEqual((w.active, w.query_token), (1, token))
        QTest.mouseClick(tabs.tabButton(tabs.currentIndex(), QTabBar.RightSide), Qt.LeftButton)
        w.actions["reopen"].trigger()
        self.wait(lambda: not w.loading)
        self.assertEqual((w.active, w.entry.text(), w.model.rowCount()), (1, 'operation = "Send"', 1))
        menu = button("Open log tab").menu()
        next(a for a in menu.actions() if a.text() == w.log_name(w.logs[other])).trigger()
        self.assertEqual(w.active, other)
        self.assertFalse(w.actions["reopen"].isEnabled())
        while tabs.count():
            w.close_tab()
        self.assertIsNone(w.active)
        w.actions["reopen"].trigger()
        self.wait(lambda: not w.loading)
        self.assertEqual(tabs.count(), 1)
        for action in w.pane_actions.values():
            action.setChecked(False)
        w.close()
        restored = Analyzer(settings)
        self.addCleanup(restored.close)
        restored.show()
        restored.load(self.path)
        self.wait(lambda: not restored.loading)
        self.assertTrue(restored.inspector.isHidden())
        for action in restored.pane_actions.values():
            action.trigger()
        self.app.processEvents()
        self.assertGreater(restored.inspector.height(), 80)
        self.assertTrue(all(size > 80 for size in restored.inspector.sizes()))

    def test_columns_fill_viewport_and_scroll_when_narrow(self):
        self.write("activity.csv", [["Operation"], *[["Review"] for _ in range(100)]])
        w = Analyzer(QSettings(str(self.path.parent / "widths.ini"), QSettings.Format.IniFormat))
        self.addCleanup(w.close)
        w.show()
        w.load(self.path)
        self.wait(lambda: not w.loading)
        table = w.table
        header = table.horizontalHeader()
        bar = table.horizontalScrollBar()

        def fitted():
            return header.length() == table.viewport().width() and bar.maximum() == 0

        w.resize(1080, 710)
        self.wait(lambda: 1000 < table.viewport().width() <= 1080 and fitted())
        initial = [table.columnWidth(i) for i in range(len(COLS))]
        w.resize(1380, 710)
        self.wait(lambda: table.viewport().width() > 1280 and fitted())
        self.assertTrue(all(table.columnWidth(i) > width for i, width in enumerate(initial)))
        self.assertGreater(table.columnWidth(3) - initial[3], table.columnWidth(0) - initial[0])
        self.assertTrue(table.verticalScrollBar().isVisible())
        w.apply("operation = Send")
        self.wait(lambda: not w.loading and fitted() and not table.verticalScrollBar().isVisible())

        w.resize(740, 750)
        self.wait(lambda: bar.isVisible() and bar.maximum() > 0)
        bar.setValue(bar.maximum())
        last = len(COLS) - 1
        self.assertLessEqual(table.columnViewportPosition(last) + table.columnWidth(last), table.viewport().width())
        self.assertTrue(all(table.columnWidth(i) >= 60 for i in range(len(COLS))))
        w.resize(1300, 710)
        self.wait(fitted)

        width = table.columnWidth(3)
        handle = QPoint(header.sectionViewportPosition(3) + width - 1, header.height() // 2)
        QTest.mousePress(header.viewport(), Qt.MouseButton.LeftButton, pos=handle)
        QTest.mouseMove(header.viewport(), handle + QPoint(50, 0))
        QTest.mouseRelease(header.viewport(), Qt.MouseButton.LeftButton, pos=handle + QPoint(50, 0))
        self.wait(lambda: fitted() and table.columnWidth(3) > width)
        chosen = table.columnWidth(3)
        table.hideColumn(2)
        self.wait(fitted)
        self.assertEqual(table.columnWidth(3), chosen)
        w.add_column("Subject", "/AuditData/Item/Subject")
        self.wait(lambda: fitted() and table.columnWidth(len(COLS)) > 0)
        self.assertEqual(header.visualIndex(len(COLS)), header.visualIndex(COLS.index("op")) + 1)
        w.remove_column(len(COLS))
        table.showColumn(2)
        self.wait(lambda: not w.loading and fitted())
        w.column_layout.reset()
        self.assertNotEqual(chosen, table.columnWidth(3))
        # Allow scrollbars to settle.
        QTest.qWait(100)
        for pane in (w.inspector.details, w.inspector.fields):
            self.wait(lambda: pane.header().length() == pane.viewport().width())
        w.open_stats("operations")
        stats = w.stats[-1]
        stats.resize(1250, 520)
        self.wait(lambda: stats.model.rowCount() and stats.table.horizontalHeader().length() == stats.table.viewport().width())
        stats.close()

    def test_verdict_actions_and_chronological_ids(self):
        settings = QSettings(str(self.path.parent / "verdicts.ini"), QSettings.Format.IniFormat)
        w = Analyzer(settings)
        self.addCleanup(w.close)
        w.show()
        w.load(self.path)
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.headerData(0, Qt.Horizontal, Qt.DisplayRole), "ID")
        self.assertEqual([w.model.index(i, 0).data() for i in range(4)], ["1", "2", "3", "4"])
        self.assertEqual(
            [w.model.record(i)["time"] for i in range(4)],
            sorted(w.model.record(i)["time"] for i in range(4)),
        )
        for i in range(4):
            self.assertIsNone(w.model.index(i, 0).data(Qt.BackgroundRole))
        headers = [
            w.model.headerData(n, Qt.Horizontal, Qt.DisplayRole) for n in range(w.model.columnCount())
        ]
        self.assertNotIn("Verdict", headers)
        for i, verdict in enumerate(("Good", "Bad", "Unsure")):
            w.table.selectRow(i)
            menu = w.actions["event_mark"].menu()
            next(action for action in menu.actions() if action.text() == verdict).trigger()
            self.wait(lambda: not w.loading)
            for n in range(w.model.columnCount()):
                color = w.model.index(i, n).data(Qt.BackgroundRole)
                self.assertEqual(color.name().upper(), VERDICT_COLORS[verdict])
            self.assertEqual(count_rows(w.db, 0, f'id = {i+1} AND verdict = "{verdict}"'), 1)
        w.apply('verdict = "Unsure"')
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.index(0, 0).data(), "3")
        w.mark_events("Unmarked")
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.rowCount(), 0)
        w.undo()
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.index(0, 0).data(), "3")
        w.apply("")
        w.sort(0)
        self.wait(lambda: not w.loading)
        w.sort(0)
        self.wait(lambda: not w.loading)
        self.assertEqual([w.model.index(i, 0).data() for i in range(4)], ["4", "3", "2", "1"])
        with closing(store.read_case(self.path)) as reader:
            self.assertEqual(count_rows(reader, 0, "marked = true"), 3)
            self.assertEqual(count_rows(reader, 0, "marked = false"), 1)

    def test_verdict_overlap_undo_and_earlier_import(self):
        targets = [("192.0.2.8", ""), ("192.0.2.9", "")]
        store.assign_verdict(self.db, "ip", targets, "Good")
        store.assign_verdict(
            self.db, "time", [(store.utc("2026-01-02"), store.utc("2026-01-03"))], "Unsure"
        )
        by_id = list(query_rows(self.db, 0, "id = 1 OR id = 2"))
        self.assertEqual(len(by_id), 2)
        store.assign_verdict(self.db, "event", [(r["id"], "") for r in by_id], "Bad")
        self.write("earlier.csv", [["Date (UTC)", "IP address"], ["2026-01-01", "192.0.2.8"], ["", ""]])
        self.assertEqual(count_rows(self.db, 0, "id = 1 AND verdict = Good"), 1)
        self.assertEqual(count_rows(self.db, 0, "(id = 2 OR id = 3) AND verdict = Bad"), 2)
        self.assertEqual(count_rows(self.db, 0, "id = 6 AND verdict = Unmarked"), 1)
        out = self.path.parent / "timeline.csv"
        export(self.db, out, 0, "verdict = Bad")
        with out.open(encoding="utf-8-sig", newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual([r["ID"] for r in rows], ["2", "3"])
        self.assertEqual({r["Verdict"] for r in rows}, {"Bad"})
        self.assertEqual({r["Source row"] for r in rows}, {"2"})
        store.undo_verdict(self.db)
        self.assertEqual(count_rows(self.db, 0, "verdict = Unsure"), 4)
        store.undo_verdict(self.db)
        self.assertEqual(count_rows(self.db, 0, "verdict = Good"), 4)
        store.undo_verdict(self.db)
        self.assertEqual(count_rows(self.db, 0, "marked = false"), 6)

    def test_inspector_column_sort_export_and_statistics(self):
        settings = QSettings(str(self.path.parent / "ui.ini"), QSettings.Format.IniFormat)
        w = Analyzer(settings)
        self.addCleanup(w.close)
        w.show()
        w.load(self.path)
        self.wait(lambda: not w.loading)
        self.assertEqual(w.windowTitle(), APP)
        self.assertEqual(w.model.rowCount(), 4)
        w.table.selectRow(next(i for i in range(4) if w.model.record(i)["op"] == "Send"))
        self.assertEqual(w.inspector.source.currentText(), "AuditData")
        w.apply('operation = "Send"')
        self.wait(lambda: not w.loading)
        ins = w.inspector
        self.assertEqual(ins.source.currentText(), "AuditData")
        ins.find.setText("needle")
        ins.find_next()
        self.assertEqual(ins.fields.currentItem().text(1), "needle")
        ins.mode.setCurrentIndex(1)
        self.assertEqual(json.loads(ins.text.toPlainText()), self.audit)
        ins.mode.setCurrentIndex(2)
        self.assertEqual(json.loads(ins.text.toPlainText()), self.audit)
        ins.source.setCurrentIndex(0)
        self.assertEqual(
            list(csv.reader(ins.text.toPlainText().splitlines()))[0], ["Operation", "AuditData"]
        )
        ins.columnRequested.emit("Subject", "/AuditData/Item/Subject")
        self.assertEqual(w.model.index(0, len(COLS)).data(), "Invoice")
        w.sort(len(COLS))
        self.wait(lambda: not w.loading)
        file = self.path.parent / "custom.csv"
        export(self.db, file, 0, w.views[0].query, order_by=w.views[0].order, custom=w.model.custom)
        with file.open(encoding="utf-8-sig", newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(rows[0]["Field /AuditData/Item/Subject"], "Invoice")
        w.apply("invalid syntax")
        self.assertEqual(w.model.rowCount(), 1)
        w.apply('session = "s1"')
        self.wait(lambda: not w.loading)
        w.open_stats("endpoints")
        s = w.stats[-1]
        self.wait(lambda: s.model.rowCount() > 0)
        self.assertEqual(s.model.rows[0][1], 2)
        self.assertEqual(s.model.index(0, 2).data(), "100.00%")
        s.apply_selected()
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.rowCount(), 2)
        self.assertIn("192.0.2.8", w.views[0].query)
        w.tab(2)
        w.tab(1)
        w.tab(3)
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.record(0)["name"], "InteractiveSignIns_AuthDetails.csv")
        w.remove_column(len(COLS))
        self.wait(lambda: not w.loading)
        self.assertEqual(w.model.columnCount(), len(COLS))
        w.home()
        self.assertEqual(w.windowTitle(), APP)

    def test_inspector_context_filters_use_clicked_values_and_log_scope(self):
        settings = QSettings(str(self.path.parent / "filters.ini"), QSettings.Format.IniFormat)
        w = Analyzer(settings)
        self.addCleanup(w.close)
        w.show()
        w.load(self.path)
        w.tab(1)
        self.wait(lambda: not w.loading)
        ins = w.inspector

        def choose(view, pos, *labels):
            chosen = []

            def trigger():
                menu = QApplication.activePopupWidget()
                try:
                    for label in labels:
                        action = next(a for a in menu.actions() if a.text() == label)
                        menu = action.menu()
                    action.trigger()
                    chosen.append(True)
                finally:
                    popup = QApplication.activePopupWidget()
                    if isinstance(popup, QMenu):
                        popup.close()

            QTimer.singleShot(0, trigger)
            event = QContextMenuEvent(QContextMenuEvent.Reason.Mouse, pos, view.viewport().mapToGlobal(pos))
            QApplication.sendEvent(view.viewport(), event)
            self.assertTrue(chosen)
            self.wait(lambda: not w.loading)

        def click_tree(view, item, *labels):
            view.scrollToItem(item)
            self.app.processEvents()
            choose(view, view.visualItemRect(item).center(), *labels)

        address = ins.details.findItems("IP address", Qt.MatchFlag.MatchRecursive)[0]
        address.parent().setExpanded(True)
        click_tree(ins.details, address, "Find matching events across case")
        self.assertEqual((w.active, w.entry.text(), w.model.rowCount()), (0, 'ip = "192.0.2.8"', 2))
        w.table.selectRow(next(i for i in range(2) if w.model.record(i)["op"] == "Send"))
        ins.find.setText("needle")
        ins.find_next()
        leaf = ins.fields.currentItem()
        expr = 'field "/AuditData/Data~1~0/*/x" = "needle"'
        click_tree(ins.fields, leaf, "Prepare a Filter", "Selected")
        self.assertEqual((w.entry.text(), w.model.rowCount()), (expr, 2))
        click_tree(ins.fields, leaf, "Apply as Filter", "… and selected")
        self.assertIn(expr, w.views[0].query)
        self.assertEqual(w.model.rowCount(), 1)
        ins.mode.setCurrentIndex(1)
        self.assertTrue(ins.text.find('"needle"'))
        choose(ins.text, ins.text.cursorRect().center(), "Apply as Filter", "Selected")
        self.assertEqual((w.entry.text(), w.model.rowCount()), (expr, 1))
        ins.mode.setCurrentIndex(0)
        obj = ins.fields.topLevelItem(0)
        click_tree(ins.fields, obj, "Apply as Filter", "Not selected")
        self.assertEqual((w.entry.text(), w.model.rowCount()), ('NOT (exists "/AuditData")', 3))
        w.tab(2)
        self.wait(lambda: not w.loading)
        ins.source.setCurrentIndex(0)
        ins.mode.setCurrentIndex(2)
        ins.text.moveCursor(QTextCursor.MoveOperation.Start)
        self.assertTrue(ins.text.find("Second"))
        choose(ins.text, ins.text.cursorRect().center(), "Apply as Filter", "Selected")
        self.assertEqual(
            (w.entry.text(), w.model.rowCount()), ('field "/@2:Incoming token type" = "Second"', 1)
        )
        w.add_column("Token", "/@2:Incoming token type")
        self.wait(lambda: not w.loading)
        cell = w.model.index(0, len(COLS))
        w.table.scrollTo(cell)
        position = w.table.visualRect(cell).center()
        choose(w.table, position, "Prepare a Filter", "Selected")
        self.assertEqual(w.entry.text(), 'field "/@2:Incoming token type" = "Second"')
        choose(w.table, position, "Follow", "Authentication request · r1")
        self.assertEqual((w.active, w.entry.text(), w.model.rowCount()), (0, 'request = "r1"', 2))


if __name__ == "__main__":
    unittest.main()
