"""Investigate Microsoft 365 email events."""

import argparse
from array import array
import contextlib
import ctypes
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
import queue
import sys
import threading

from PySide6.QtCore import QSettings, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QCloseEvent, QFontDatabase, QIcon, QKeySequence
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QInputDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QSplitter,
    QStackedWidget,
    QTableView,
    QToolBar,
    QToolButton,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from bql import FIELDS, Query, utc
import case_store as store
from desktop_style import (
    APP,
    COLS,
    LABELS,
    TIMELINE_SIZES,
    ColumnLayout,
    VERDICT_COLORS,
    EventTable,
    LogTabs,
    VerdictDelegate,
    icon,
    style,
)
from event_fields import EventFields, SLOTS, pointer_values
from record_view import Inspector, compact_layout, copy, tree
from statistics_window import Statistics
from telemetry import REPORTS, combine, quality, quote


@dataclass
class View:
    query: str = ""
    draft: str = ""
    order: str = "time"
    desc: bool = False
    selected: int | None = None
    scroll: int = 0


EXAMPLES = [
    ("Marked events", "marked = true"),
    ("Failed sign-ins", 'source contains "sign-ins" AND result = "Failure"'),
    ("Sent messages", 'operation = "Send"'),
    ("Mailbox access", 'operation = "MailItemsAccessed" OR operation = "AttachmentAccess"'),
    ("Inbox rules and forwarding", 'operation contains "InboxRule" OR operation = "Set-Mailbox"'),
    ("Consent and permissions", REPORTS["consent"].subset),
    ("Device changes", 'source = "Entra directory audit" AND operation contains "device"'),
]


class Analyzer(QMainWindow):
    def __init__(self, settings=None):
        super().__init__()
        self.setWindowTitle(APP)
        self.resize(1370, 830)
        self.setMinimumSize(720, 440)
        self.settings = settings if settings is not None else QSettings("M365-BEC", "Email Log Analyzer")
        self.db = None
        self.path = None
        self.logs = {}
        self.views = {}
        self.closed_tabs = []
        self.active = None
        self.history = []
        self.jobs = queue.Queue()
        self.worker = None
        self.query_token = 0
        self.cancel = threading.Event()
        self.stats = []
        self.loading = False
        self.actions = {}
        self.model = EventTable()
        self._menus()
        self._toolbar()
        self._central()
        self._status()
        self._welcome()
        if self.settings.contains("geometry"):
            self.restoreGeometry(self.settings.value("geometry"))
        self.layout_action.setChecked(self.settings.value("inspectorVertical", False, type=bool))
        self.layout_panes()
        if self.settings.contains("mainSplit"):
            self.split.restoreState(self.settings.value("mainSplit"))
        if self.settings.contains("detailSplit"):
            self.inspector.restoreState(self.settings.value("detailSplit"))
        for key, action in self.pane_actions.items():
            action.setChecked(self.settings.value(key + "PaneVisible", True, type=bool))
        self.show_panes()
        self.color_action.setChecked(self.settings.value("verdictColors", True, type=bool))
        self.toggle_colors()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(55)
        self.setAcceptDrops(True)
        self.update_actions()
        if sys.platform == "win32":
            QTimer.singleShot(0, self.caption)

    def caption(self):
        for attr, val in ((35, 0x008BCCAA), (36, 0x00192014)):
            color = ctypes.c_uint(val)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                ctypes.c_void_p(int(self.winId())), attr, ctypes.byref(color), 4
            )

    def action(self, menu, key, label, func, shortcut=None, glyph=None, color="#c2c7c9"):
        a = QAction(label, self)
        a.triggered.connect(lambda checked=False: func())
        if shortcut:
            a.setShortcut(QKeySequence(shortcut))
        if glyph:
            a.setIcon(icon(glyph, color))
        menu.addAction(a)
        self.actions[key] = a
        return a

    def _menus(self):
        bar = self.menuBar()
        self._file_menu(bar.addMenu("&File"))
        self._edit_menu(bar.addMenu("&Edit"))
        self._view_menu(bar.addMenu("&View"))
        self._go_menu(bar.addMenu("&Go"))
        self._analyze_menu(bar.addMenu("&Analyze"))
        self._statistics_menu(bar.addMenu("&Statistics"))
        help_menu = bar.addMenu("&Help")
        self.action(help_menu, "help", "BQL display filter reference…", self.help, "F1")

    def _file_menu(self, menu):
        self.action(menu, "open", "Open logs…", self.open_files, "Ctrl+O", 0xE8E5, "#e8bd61")
        self.action(menu, "new", "New case…", self.new_case, "Ctrl+N")
        self.action(menu, "case", "Open case…", self.open_case, "Ctrl+Shift+O", 0xE8B7, "#98bfe2")
        self.recent_menu = menu.addMenu("Recent cases")
        self.recent_menu.aboutToShow.connect(self.populate_recent)
        menu.addSeparator()
        self.action(menu, "export", "Export displayed events…", self.export_view, "Ctrl+Shift+E", 0xE74E)
        self.action(menu, "extract", "Save original evidence copy…", self.extract_original)
        self.action(menu, "close", "Close tab", self.close_tab, "Ctrl+W", 0xE8BB)
        menu.addSeparator()
        self.action(menu, "quit", "Exit", self.close, "Alt+F4")

    def _edit_menu(self, menu):
        self.action(menu, "copy", "Copy event with source reference", self.copy_event, "Ctrl+Shift+C")
        for key, title, apply, glyph in (
            ("event_mark", "Mark selected events", self.mark_events, 0xE7C1),
            ("mark", "Mark selected IPs", self.mark_ip, 0xE774),
            ("range", "Mark UTC time range", self.mark_time, 0xE916),
        ):
            submenu = menu.addMenu(title)
            submenu.setIcon(icon(glyph))
            self.actions[key] = submenu.menuAction()
            for verdict in store.VERDICTS:
                label = "Clear verdict" if verdict == "Unmarked" else verdict
                action = submenu.addAction(label)
                if verdict in VERDICT_COLORS:
                    action.setIcon(icon(0xE91F, VERDICT_COLORS[verdict]))
                action.triggered.connect(lambda checked=False, v=verdict, fn=apply: fn(v))
        self.action(menu, "undo", "Undo last mark", self.undo, "Ctrl+Z", 0xE7A7)

    def _view_menu(self, menu):
        self.action(menu, "home", "Welcome screen", self.home, None, 0xE80F)
        self.action(menu, "all", "All case events", lambda: self.tab(0), "Ctrl+Shift+A", 0xE8A9, "#99cbe6")
        self.log_menu = menu.addMenu("Open log tab")
        self.action(menu, "reopen", "Reopen closed tab", self.reopen_tab, "Ctrl+Shift+T")
        menu.addSeparator()
        self.pane_actions = {}
        for key, label in (("details", "Event details"), ("source", "Source record")):
            action = menu.addAction(label)
            action.setCheckable(True)
            action.setChecked(True)
            action.toggled.connect(self.show_panes)
            self.pane_actions[key] = action
        self.color_action = self.action(
            menu, "colors", "Show verdict colors", self.toggle_colors, None, 0xE790
        )
        self.color_action.setCheckable(True)
        self.color_action.setChecked(True)
        self.layout_action = self.action(menu, "stack", "Stack inspector panes vertically", self.layout_panes)
        self.layout_action.setCheckable(True)
        self.action(menu, "zoomIn", "Zoom in", lambda: self.zoom(1), "Ctrl++", 0xE8A3)
        self.action(menu, "zoomOut", "Zoom out", lambda: self.zoom(-1), "Ctrl+-", 0xE71F)
        self.action(menu, "zoomReset", "Actual size", lambda: self.zoom(0), "Ctrl+0")

    def _go_menu(self, menu):
        for key, label, step, shortcut, glyph in [
            ("previous", "Previous event", -1, "Alt+Up", 0xE72B),
            ("next", "Next event", 1, "Alt+Down", 0xE72A),
            ("first", "First displayed event", "first", "Ctrl+Home", 0xE74A),
            ("last", "Last displayed event", "last", "Ctrl+End", 0xE74B),
        ]:
            self.action(menu, key, label, lambda s=step: self.navigate(s), shortcut, glyph)

    def _analyze_menu(self, menu):
        self.action(menu, "focus", "Display filter", self.focus_filter, "Ctrl+/", 0xE721, "#9fc8e9")
        self.action(menu, "find", "Find in source record", self.find_record, "Ctrl+F")
        self.action(menu, "clear", "Clear display filter", lambda: self.apply(""), "Ctrl+Shift+L")
        self.action(menu, "save_filter", "Save display filter in case…", self.save_filter, "Ctrl+Shift+S")
        self.action(menu, "saved_filters", "Saved display filters…", self.manage_filters)
        self.action(menu, "quality", "Evidence and import details…", self.import_details, None, 0xE946)

    def _statistics_menu(self, menu):
        for key in (
            "activity",
            "indicators",
            "sources",
            "files",
            "operations",
            "endpoints",
            "mailboxes",
            "ip_accounts",
            "conversations",
        ):
            self.action(
                menu,
                "stat_" + key,
                REPORTS[key].title + "…",
                lambda k=key: self.open_stats(k),
                "Ctrl+Shift+I" if key == "activity" else None,
                0xE9D9 if key == "activity" else None,
            )
        for title, keys in [
            (
                "Identity and authentication",
                (
                    "outcomes",
                    "devices",
                    "sessions",
                    "locations",
                    "applications",
                    "clients",
                    "methods",
                    "policies",
                ),
            ),
            (
                "Mailbox and directory",
                ("rules", "access", "folders", "items", "delivery", "consent", "marks"),
            ),
        ]:
            sub = menu.addMenu(title)
            for key in keys:
                self.action(sub, "stat_" + key, REPORTS[key].title + "…", lambda k=key: self.open_stats(k))

    def _toolbar(self):
        tb = QToolBar("Log tools", self)
        self.addToolBar(tb)
        tb.setMovable(False)
        tb.setIconSize(QSize(20, 20))
        for key in (
            "open",
            "case",
            "close",
            None,
            "home",
            "all",
            None,
            "previous",
            "next",
            "first",
            "last",
            None,
            "focus",
            "colors",
            "stat_activity",
            "quality",
            None,
            "event_mark",
            "mark",
            "range",
            "undo",
            None,
            "export",
            "zoomIn",
            "zoomOut",
        ):
            if key:
                tb.addAction(self.actions[key])
                if key in ("event_mark", "mark", "range"):
                    tb.widgetForAction(self.actions[key]).setPopupMode(
                        QToolButton.ToolButtonPopupMode.InstantPopup
                    )
            else:
                tb.addSeparator()

    def _central(self):
        base = QWidget()
        self.setCentralWidget(base)
        outer = compact_layout(base)
        frame = QFrame()
        frame.setObjectName("filterBar")
        fl = compact_layout(frame, True, 3)
        examples = QToolButton()
        examples.setIcon(icon(0xE8A4, "#91b9df"))
        examples.setToolTip("Filter expressions and history")
        self.filters = QMenu(examples)
        self.filters.aboutToShow.connect(self.filter_menu)
        examples.setMenu(self.filters)
        examples.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        fl.addWidget(examples)
        self.entry = QLineEdit()
        self.entry.setMaxLength(2**31 - 1)
        self.entry.setAccessibleName("BQL display filter")
        self.entry.setPlaceholderText("Apply a BQL display filter …  <Ctrl+/>")
        self.entry.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        self.entry.returnPressed.connect(lambda: self.apply())
        self.entry.textChanged.connect(self.validate)
        fl.addWidget(self.entry, 1)
        for code, tip, fn in [
            (0xE72A, "Apply display filter", lambda: self.apply()),
            (0xE711, "Clear display filter", lambda: self.apply("")),
        ]:
            b = QToolButton()
            b.setIcon(icon(code, "#a5cbed"))
            b.setToolTip(tip)
            b.clicked.connect(fn)
            fl.addWidget(b)
        outer.addWidget(frame)
        self.pages = QStackedWidget()
        outer.addWidget(self.pages, 1)
        self.welcome = QWidget()
        self.pages.addWidget(self.welcome)
        work = QWidget()
        wl = compact_layout(work)
        self.pages.addWidget(work)
        self.tabs = LogTabs()
        self.tabs.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        self.tabs.setExpanding(False)
        self.tabs.setMovable(True)
        self.tabs.setUsesScrollButtons(True)
        self.tabs.setElideMode(Qt.TextElideMode.ElideMiddle)
        self.tabs.currentChanged.connect(self.switched)
        self.tabs.tabCloseRequested.connect(self.close_index)
        tab_strip = QHBoxLayout()
        tab_strip.setContentsMargins(0, 0, 0, 0)
        tab_strip.setSpacing(2)
        tab_strip.addWidget(self.tabs, 1)
        open_tab = QToolButton()
        open_tab.setText("+")
        open_tab.setToolTip("Open log tab")
        open_tab.setAccessibleName("Open log tab")
        open_tab.setMenu(self.log_menu)
        open_tab.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        tab_strip.addWidget(open_tab)
        wl.addLayout(tab_strip)
        self.split = QSplitter(Qt.Orientation.Vertical)
        self.split.setChildrenCollapsible(False)
        wl.addWidget(self.split, 1)
        self.table = QTableView()
        self.table.setObjectName("timeline")
        self.table.setItemDelegate(VerdictDelegate(self.table))
        self.table.setModel(self.model)
        self.table.setShowGrid(False)
        self.table.setAlternatingRowColors(True)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.table.setWordWrap(False)
        self.table.setHorizontalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(21)
        self.table.verticalHeader().setMinimumSectionSize(16)
        header = self.table.horizontalHeader()
        header.setSectionsClickable(True)
        header.setSectionsMovable(True)
        header.setDefaultAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        header.setSortIndicatorShown(True)
        header.sectionClicked.connect(self.sort)
        header.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        header.customContextMenuRequested.connect(self.column_menu)
        self.column_layout = ColumnLayout(self.table, TIMELINE_SIZES)
        self.table.selectionModel().currentRowChanged.connect(self.inspect)
        self.table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.table.customContextMenuRequested.connect(self.context)
        self.split.addWidget(self.table)
        self.inspector = Inspector()
        self.inspector.paneCloseRequested.connect(lambda key: self.pane_actions[key].setChecked(False))
        self.split.addWidget(self.inspector)
        self.inspector.filterMenuRequested.connect(self.add_filter_actions)
        self.inspector.followEvent.connect(self.follow_event)
        self.inspector.columnRequested.connect(self.add_column)
        self.split.setSizes([430, 310])
        self.inspector.setSizes([520, 750])

    def _status(self):
        self.scope_label = QLabel("No logs open")
        self.scope_label.setContentsMargins(10, 0, 10, 0)
        self.zone = QLabel("Time: UTC")
        self.zone.setContentsMargins(10, 0, 8, 0)
        self.warnings = QToolButton()
        self.warnings.setStyleSheet("color:#f0c68d")
        self.warnings.setToolTip("Open import warnings and evidence coverage")
        self.warnings.clicked.connect(self.import_details)
        self.warnings.hide()
        self.statusBar().addPermanentWidget(self.warnings)
        self.statusBar().addPermanentWidget(self.scope_label)
        self.statusBar().addPermanentWidget(self.zone)
        self.statusBar().showMessage("Ready to open email logs")

    def _welcome(self):
        layout = QVBoxLayout(self.welcome)
        layout.setContentsMargins(56, 34, 48, 24)
        layout.setSpacing(12)
        banner = QLabel("Welcome to " + APP)
        banner.setStyleSheet("background:#b94f00;color:#ffffff;padding:7px 11px;border-radius:3px")
        layout.addWidget(banner, 0, Qt.AlignmentFlag.AlignLeft)
        row = QHBoxLayout()
        row.setSpacing(32)
        layout.addLayout(row)
        left = QWidget()
        ll = compact_layout(left)

        def heading(parent, text):
            lab = QLabel(text)
            lab.setStyleSheet("font-size:17px;font-weight:600;color:#bcc3c5;padding:14px 0 6px")
            parent.addWidget(lab)

        heading(ll, "Open")
        buttons = QHBoxLayout()
        for text, fn in [
            ("Open logs…", self.open_files),
            ("Open case…", self.open_case),
            ("New case…", self.new_case),
        ]:
            btn = QPushButton(text)
            btn.clicked.connect(fn)
            buttons.addWidget(btn)
        buttons.addStretch()
        ll.addLayout(buttons)
        heading(ll, "Recent cases")
        self.recent = QListWidget()
        self.recent.setFrameShape(QFrame.Shape.NoFrame)
        self.recent.setAlternatingRowColors(True)
        self.recent.itemActivated.connect(lambda item: self.load(item.data(Qt.ItemDataRole.UserRole)))
        ll.addWidget(self.recent)
        self.show_recent()
        row.addWidget(left, 2)
        right = QWidget()
        rl = compact_layout(right)
        heading(rl, "Display filters")
        for label, expr in EXAMPLES[:5]:
            btn = QPushButton(label)
            btn.setFlat(True)
            btn.setStyleSheet("text-align:left;color:#aacdeb;padding:4px")
            btn.clicked.connect(lambda checked=False, s=expr: self.apply(s))
            rl.addWidget(btn)
        rl.addStretch()
        row.addWidget(right, 1)
        layout.addStretch(1)

    def recent_paths(self):
        return self.settings.value("recentCases", [], type=list) or []

    def remember(self):
        paths = [str(self.path)] + [p for p in self.recent_paths() if p != str(self.path)]
        self.settings.setValue("recentCases", paths[:8])
        self.show_recent()

    def show_recent(self):
        self.recent.clear()
        for path in self.recent_paths():
            item = QListWidgetItem(icon(0xE8B7, "#97bad5"), path)
            item.setData(Qt.ItemDataRole.UserRole, path)
            self.recent.addItem(item)

    def populate_recent(self):
        self.recent_menu.clear()
        for path in self.recent_paths():
            self.recent_menu.addAction(path, lambda checked=False, p=path: self.load(p))
        if self.recent_paths():
            self.recent_menu.addSeparator()
            self.recent_menu.addAction("Clear recent cases", self.clear_recent)

    def clear_recent(self):
        self.settings.remove("recentCases")
        self.show_recent()

    def filter_menu(self):
        self.filters.clear()
        for name, expr in self.saved_filters():
            self.filters.addAction(name, lambda checked=False, s=expr: self.apply(s))
        if not self.filters.isEmpty():
            self.filters.addSeparator()
        for label, expr in EXAMPLES:
            self.filters.addAction(label, lambda checked=False, s=expr: self.apply(s))
        if self.history:
            self.filters.addSeparator()
            for expr in self.history:
                self.filters.addAction(expr, lambda checked=False, s=expr: self.apply(s))
        self.filters.addSeparator()
        self.filters.addAction("BQL reference…", self.help)

    def saved_filters(self):
        if (
            self.db is None
            or not self.db.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='display_filters'"
            ).fetchone()
        ):
            return []
        return self.db.execute(
            "SELECT name,expression FROM display_filters ORDER BY name COLLATE NOCASE"
        ).fetchall()

    def save_filter(self):
        if self.db is None or self.busy():
            return
        expr = self.entry.text().strip()
        if not expr:
            QMessageBox.information(self, APP, "Enter a display filter first.")
            return
        try:
            Query(expr)
        except (ValueError, RecursionError) as exc:
            QMessageBox.warning(self, "Save filter", str(exc))
            return
        name, ok = QInputDialog.getText(self, "Save display filter", "Name")
        name = name.strip()
        if not ok or not name:
            return
        if any(n.casefold() == name.casefold() for n, _ in self.saved_filters()):
            if (
                QMessageBox.question(self, "Save display filter", "Replace the existing named filter?")
                != QMessageBox.StandardButton.Yes
            ):
                return
        with self.db:
            self.db.execute(
                "CREATE TABLE IF NOT EXISTS display_filters(name TEXT PRIMARY KEY COLLATE NOCASE,expression TEXT NOT NULL)"
            )
            self.db.execute("INSERT OR REPLACE INTO display_filters VALUES(?,?)", (name, expr))
        self.statusBar().showMessage("Saved display filter · " + name)

    def manage_filters(self):
        if self.db is None:
            return
        dlg = QDialog(self)
        dlg.setWindowTitle("Saved display filters")
        dlg.resize(680, 330)
        layout = QVBoxLayout(dlg)
        view = tree(["Name", "BQL expression"])
        view.setRootIsDecorated(False)
        layout.addWidget(view)
        for name, expr in self.saved_filters():
            QTreeWidgetItem(view, [name, expr])
        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        apply = btns.addButton("Apply", QDialogButtonBox.ButtonRole.ActionRole)
        remove = btns.addButton("Delete", QDialogButtonBox.ButtonRole.ActionRole)

        def use():
            n = view.currentItem()
            if n:
                dlg.accept()
                self.apply(n.text(1))

        def delete():
            n = view.currentItem()
            if (
                n
                and not self.busy()
                and QMessageBox.question(dlg, "Delete display filter", "Delete " + n.text(0) + "?")
                == QMessageBox.StandardButton.Yes
            ):
                with self.db:
                    self.db.execute("DELETE FROM display_filters WHERE name=?", (n.text(0),))
                view.takeTopLevelItem(view.indexOfTopLevelItem(n))

        apply.clicked.connect(use)
        remove.clicked.connect(delete)
        view.itemDoubleClicked.connect(lambda item, col: use())
        btns.rejected.connect(dlg.reject)
        layout.addWidget(btns)
        dlg.exec()

    def focus_filter(self):
        self.entry.setFocus()
        self.entry.selectAll()

    def find_record(self):
        self.pane_actions["source"].setChecked(True)
        self.inspector.find.setFocus()
        self.inspector.find.selectAll()

    def update_actions(self):
        active = self.active is not None and self.db is not None
        for k, a in self.actions.items():
            if k.startswith("stat_") or k in (
                "export",
                "close",
                "copy",
                "event_mark",
                "mark",
                "range",
                "previous",
                "next",
                "first",
                "last",
                "find",
                "extract",
            ):
                a.setEnabled(active)
            elif k in ("all", "quality", "undo", "save_filter", "saved_filters"):
                a.setEnabled(bool(self.db))
        self.actions["reopen"].setEnabled(bool(self.closed_tabs))

    def busy(self):
        if self.worker is not None:
            QMessageBox.information(self, APP, "A file operation is still running.")
            return True
        return False

    def load(self, path, new=False):
        if self.busy():
            return False
        path = Path(path).resolve()
        if not new and not path.is_file():
            QMessageBox.warning(self, "Open case", "Case file could not be found.")
            return False
        try:
            db = store.connect(path)
        except Exception as exc:
            QMessageBox.warning(self, "Open case", str(exc))
            return False
        self.cancel_query()
        self.model.reset(None, [])
        self.inspector.clear_record()
        for dlg in list(self.stats):
            dlg.close()
        if self.db:
            self.db.close()
        while self.model.custom:
            self.model.remove_field(self.model.columnCount() - 1)
        self.db = db
        self.path = path
        self.active = None
        self.views = {}
        self.closed_tabs.clear()
        self.history = []
        self.tabs.blockSignals(True)
        while self.tabs.count():
            self.tabs.removeTab(0)
        self.tabs.blockSignals(False)
        self.refresh_logs()
        self.remember()
        if self.logs:
            self.make_tab(0)
            for key in self.logs:
                self.make_tab(key)
            self.tab(0)
        else:
            self.home()
        return True

    def new_case(self):
        if self.busy():
            return False
        path, _ = QFileDialog.getSaveFileName(
            self, "Create a single-tenant case", "", "Investigation case (*.bec)"
        )
        if not path:
            return False
        if Path(path).exists():
            QMessageBox.warning(self, APP, "Choose an unused name, or use Open case.")
            return False
        return self.load(path, True)

    def open_case(self):
        if self.busy():
            return
        path, _ = QFileDialog.getOpenFileName(self, "Open case", "", "Investigation case (*.bec)")
        if path:
            self.load(path)

    def open_files(self):
        if self.busy():
            return
        files, _ = QFileDialog.getOpenFileNames(
            self, "Open logs from one tenant", "", "Log files (*.csv *.xlsx)"
        )
        if files and (self.db or self.new_case()):
            self.import_paths(files)

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls() and all(
            Path(u.toLocalFile()).suffix.lower() in (".csv", ".xlsx", ".bec")
            for u in event.mimeData().urls()
        ):
            event.acceptProposedAction()

    def dropEvent(self, event):
        files = [u.toLocalFile() for u in event.mimeData().urls()]
        if len(files) == 1 and files[0].lower().endswith(".bec"):
            self.load(files[0])
        elif any(p.lower().endswith(".bec") for p in files):
            QMessageBox.information(self, APP, "Open one case, then drop its log files.")
        elif files and not self.busy() and (self.db or self.new_case()):
            self.import_paths(files)

    def refresh_logs(self):
        self.logs = {r["id"]: r for r in self.db.execute("SELECT * FROM logs ORDER BY id")}
        self.log_menu.clear()
        self.log_menu.addAction(self.actions["all"])
        self.log_menu.addSeparator()
        for key, log in self.logs.items():
            self.log_menu.addAction(self.log_name(log), lambda checked=False, k=key: self.tab(k))

    @staticmethod
    def log_name(log):
        return log["name"] + (" / " + log["sheet"] if log["sheet"] else "")

    def make_tab(self, key):
        if key not in self.views:
            self.views[key] = View()
        for i in range(self.tabs.count()):
            if self.tabs.tabData(i) == key:
                return i
        self.tabs.blockSignals(True)
        label = "All case events" if not key else self.logs[key]["sheet"] or self.logs[key]["kind"]
        if key and any(self.tabs.tabText(i) == label for i in range(self.tabs.count())):
            label += f" ({key})"
        i = self.tabs.addTab(label)
        self.tabs.setTabData(i, key)
        self.tabs.setTabToolTip(i, self.log_name(self.logs[key]) if key else "All imported files")
        self.tabs.add_close_button(i, "Close " + label)
        if key in self.closed_tabs:
            self.closed_tabs.remove(key)
        self.tabs.blockSignals(False)
        return i

    def remember_view(self):
        if self.active is None:
            return
        v = self.views[self.active]
        v.draft = self.entry.text()
        index = self.table.currentIndex()
        if index.isValid() and self.model.ids:
            v.selected = self.model.ids[index.row()]
            v.scroll = self.table.verticalScrollBar().value()

    def tab(self, key):
        if self.db is None or key and key not in self.logs:
            return
        i = self.make_tab(key)
        self.tabs.blockSignals(True)
        self.tabs.setCurrentIndex(i)
        self.tabs.blockSignals(False)
        self.switched(i)

    def switched(self, i):
        if i < 0:
            return
        self.remember_view()
        self.active = self.tabs.tabData(i)
        self.pages.setCurrentIndex(1)
        self.entry.setText(self.views[self.active].draft)
        self.update_actions()
        self.refresh()

    def close_index(self, i):
        if not 0 <= i < self.tabs.count():
            return
        key = self.tabs.tabData(i)
        if key == self.active:
            self.remember_view()
            self.active = None
        self.tabs.blockSignals(True)
        self.tabs.removeTab(i)
        self.tabs.blockSignals(False)
        self.closed_tabs.append(key)
        if not self.tabs.count():
            self.home()
        elif self.active != self.tabs.tabData(self.tabs.currentIndex()):
            self.switched(self.tabs.currentIndex())
        else:
            self.update_actions()

    def reopen_tab(self):
        if self.closed_tabs:
            self.tab(self.closed_tabs[-1])

    def close_tab(self):
        if self.active is not None:
            self.close_index(self.tabs.currentIndex())

    def home(self):
        self.remember_view()
        self.active = None
        self.cancel_query()
        self.model.reset(None, [])
        self.inspector.clear_record()
        self.pages.setCurrentIndex(0)
        self.entry.clear()
        self.statusBar().showMessage("Ready to open email logs")
        self.warnings.hide()
        self.scope_label.setText(f"{len(self.logs)} logs in case" if self.db else "No case open")
        self.update_actions()

    def validate(self, text):
        try:
            Query(text)
            state = "valid" if text.strip() else ""
            hint = "Press Enter to apply"
        except (ValueError, RecursionError) as exc:
            state = "error"
            hint = str(exc)
        self.entry.setToolTip(hint)
        if self.entry.property("validity") != state:
            self.entry.setProperty("validity", state)
            self.entry.style().unpolish(self.entry)
            self.entry.style().polish(self.entry)

    def apply(self, value=None):
        text = self.entry.text() if value is None else value
        self.entry.setText(text)
        try:
            Query(text)
        except (ValueError, RecursionError) as exc:
            self.statusBar().showMessage(str(exc))
            return
        if not self.db:
            self.statusBar().showMessage("Open a case or import logs to apply this filter.")
            return
        if self.active is None:
            self.tab(0)
        self.entry.setText(text)
        v = self.views[self.active]
        v.query = v.draft = text.strip()
        v.selected = None
        v.scroll = 0
        if text:
            self.history = ([text] + [s for s in self.history if s != text])[:10]
        self.refresh()

    def apply_in(self, log, expr):
        if log != self.active:
            self.tab(log)
        self.apply(expr)
        self.raise_()
        self.activateWindow()

    def cancel_query(self):
        self.cancel.set()
        self.query_token += 1
        self.loading = False

    def refresh(self):
        if self.active is None:
            return
        self.cancel_query()
        self.cancel = threading.Event()
        cancel = self.cancel
        token = self.query_token
        self.model.reset(self.db, [])
        self.inspector.clear_record()
        self.loading = True
        v = self.views[self.active]
        log = self.active
        path = self.path
        clause, params = store.where(log, v.query)
        order, ordering = store.sort_sql(v.order, v.desc)
        sql = "SELECT e.id" + store.EVENT_JOIN + clause + order
        params += ordering
        keys = list(COLS) + ["ptr:" + p for _, p in self.model.custom]
        self.table.horizontalHeader().setSortIndicator(
            keys.index(v.order), Qt.SortOrder.DescendingOrder if v.desc else Qt.SortOrder.AscendingOrder
        )
        self.scope_label.setText("Loading displayed events…")

        def run():
            try:
                with contextlib.closing(store.read_case(path)) as db:
                    db.set_progress_handler(lambda: int(cancel.is_set()), 1200)
                    ids = array("q", (r[0] for r in db.execute(sql, params)))
                if not cancel.is_set():
                    self.jobs.put(("query", token, ids))
            except Exception as exc:
                if not cancel.is_set():
                    self.jobs.put(("query_error", token, str(exc)))

        threading.Thread(target=run, daemon=True).start()

    def show_rows(self, ids):
        v = self.views[self.active]
        self.loading = False
        self.model.reset(self.db, ids)
        if ids:
            try:
                pos = ids.index(v.selected) if v.selected is not None else 0
            except ValueError:
                pos = 0
            scroll = v.scroll
            self.table.selectRow(pos)
            if scroll:
                self.table.verticalScrollBar().setValue(scroll)
            self.inspect()
        total = (
            self.logs[self.active]["total"] if self.active else sum(l["total"] for l in self.logs.values())
        )
        self.scope_label.setText(f"Events: {total:,}    Displayed: {len(ids):,}")
        warnings = [
            l for key, l in self.logs.items() if l["warning"] and (not self.active or key == self.active)
        ]
        self.warnings.setText(f"⚠  Export limits: {len(warnings)}")
        self.warnings.setVisible(bool(warnings))
        label = self.log_name(self.logs[self.active]) if self.active else "All case events"
        self.scope_label.setToolTip(str(self.path))
        self.statusBar().showMessage(label)

    def poll(self):
        while not self.jobs.empty():
            kind, token, data = self.jobs.get()
            if kind.startswith("query"):
                if token != self.query_token:
                    continue
                if kind == "query":
                    self.show_rows(data)
                else:
                    self.loading = False
                    self.scope_label.setText("Query failed")
                    self.statusBar().showMessage(data)
            elif kind == "progress":
                self.statusBar().showMessage(data)
            else:
                self.worker = None
                if kind == "error":
                    QMessageBox.warning(self, APP, data)
                elif kind == "import":
                    self.import_finished(data)
                else:
                    self.statusBar().showMessage("Saved " + str(data))

    def sort(self, column):
        if self.active is None:
            return
        self.remember_view()
        v = self.views[self.active]
        name = COLS[column] if column < len(COLS) else "ptr:" + self.model.custom[column - len(COLS)][1]
        v.desc = not v.desc if v.order == name else False
        v.order = name
        self.refresh()

    def selected_rows(self):
        return [self.model.record(i.row()) for i in self.table.selectionModel().selectedRows()]

    def current(self):
        return self.model.record(self.table.currentIndex().row())

    def inspect(self, *_):
        row = self.current()
        if row:
            self.inspector.set_record(
                self.db, row["id"], row["source"], row["reason"], row["verdict"], row["timeline_id"]
            )
        else:
            self.inspector.clear_record()

    def follow_event(self, eid):
        r = self.db.execute("SELECT log,rownum FROM events WHERE id=?", (eid,)).fetchone()
        if r:
            self.apply_in(r["log"], f"row = {r['rownum']}")

    def navigate(self, step):
        if not self.model.ids:
            return
        pos = self.table.currentIndex().row()
        pos = 0 if step == "first" else len(self.model.ids) - 1 if step == "last" else pos + step
        self.table.selectRow(max(0, min(pos, len(self.model.ids) - 1)))

    def toggle_colors(self):
        self.model.colors = self.color_action.isChecked()
        self.table.viewport().update()

    def layout_panes(self):
        self.inspector.setOrientation(
            Qt.Orientation.Vertical if self.layout_action.isChecked() else Qt.Orientation.Horizontal
        )
        self.inspector.setSizes([400, 550])

    def show_panes(self):
        for i, key in enumerate(("details", "source")):
            self.inspector.widget(i).setVisible(self.pane_actions[key].isChecked())
        self.inspector.setVisible(any(action.isChecked() for action in self.pane_actions.values()))

    def zoom(self, delta):
        font = self.table.font()
        pt = max(7, min(16, font.pointSize() + delta)) if delta else 9
        font.setPointSize(pt)
        self.model.font.setPointSize(pt)
        self.table.setFont(font)
        self.table.verticalHeader().setDefaultSectionSize(pt * 2 + 3)
        self.table.viewport().update()

    def column_menu(self, pos):
        menu = QMenu(self)
        for i, label in enumerate(list(LABELS) + [label for label, path in self.model.custom]):
            action = menu.addAction(label)
            action.setCheckable(True)
            action.setChecked(not self.table.isColumnHidden(i))
            action.toggled.connect(lambda on, col=i: self.table.setColumnHidden(col, not on))
        header = self.table.horizontalHeader()
        menu.addSeparator()
        menu.addAction("Fit columns to window", self.column_layout.reset)
        n = header.logicalIndexAt(pos)
        if n >= len(COLS):
            menu.addSeparator()
            menu.addAction("Remove custom column", lambda: self.remove_column(n))
        menu.exec(self.table.horizontalHeader().mapToGlobal(pos))

    def add_column(self, label, path):
        self.model.append_field(label, path)
        n = len(COLS) + next(i for i, (_, p) in enumerate(self.model.custom) if p == path)
        self.table.showColumn(n)
        head = self.table.horizontalHeader()
        head.moveSection(head.visualIndex(n), head.visualIndex(COLS.index("op")) + 1)
        self.statusBar().showMessage("Added column · " + path)

    def remove_column(self, n):
        key = "ptr:" + self.model.custom[n - len(COLS)][1]
        for v in self.views.values():
            if v.order == key:
                v.order = "time"
                v.desc = False
        self.model.remove_field(n)
        self.refresh()

    def context(self, pos):
        index = self.table.indexAt(pos)
        if not index.isValid():
            return
        if not self.table.selectionModel().isRowSelected(index.row(), index.parent()):
            self.table.selectRow(index.row())
        record = self.model.record(index.row())
        headers, raw, request = self.db.execute(
            "SELECT l.headers,e.raw,e.request FROM events e JOIN logs l ON l.id=e.log WHERE e.id=?",
            (record["id"],),
        ).fetchone()
        column = index.column()
        key = COLS[column] if column < len(COLS) else f"custom_{column - len(COLS)}"
        value = record[key]
        menu = QMenu(self)
        for action in ("event_mark", "mark", "range"):
            menu.addAction(self.actions[action])
        menu.addSeparator()
        if column >= len(COLS):
            pointer = self.model.custom[column - len(COLS)][1]
            field = "field " + quote(pointer)
            values = dict.fromkeys(pointer_values(headers, raw, pointer))
        else:
            field = {"timeline_id": "id", "name": "file", "op": "operation"}.get(key, key)
            values = filter(None, value.split(", ") if key == "ip" else [value])
        expression = " OR ".join(f"{field} = {quote(item)}" for item in values)
        if expression:
            self.add_filter_actions(menu, expression)
        self._populate_follow_menu(menu.addMenu("Follow"), record["actor"], request, EventFields(headers, raw))
        menu.addSeparator()
        copy_menu = menu.addMenu("Copy")
        copy_menu.addAction("Cell value", lambda: copy(value))
        copy_menu.addAction(self.actions["copy"])
        reference = (record["name"], record["sheet"], f"row {record['rownum']}")
        copy_menu.addAction("Source reference", lambda: copy(" · ".join(filter(None, reference))))
        menu.exec(self.table.viewport().mapToGlobal(pos))

    def _populate_follow_menu(self, menu, actor, request, fields):
        for field, label, values in (
            ("mailbox", "Account", [actor]),
            ("session", "Session", fields.values("session")),
            ("device", "Device", fields.values("device")),
            ("request", "Authentication request", [request]),
            ("messageid", "Message", fields.values("messageid")),
            ("correlation", "Correlation", fields.values("correlation")),
        ):
            for value in filter(None, values):
                expression = f"{field} = {quote(value)}"
                menu.addAction(
                    label + " · " + value[:55],
                    lambda checked=False, query=expression: self.apply_in(0, query),
                )
        menu.setEnabled(not menu.isEmpty())

    def add_filter_actions(self, menu, expr):
        current = self.views[self.active].query
        excluded = "NOT (" + expr + ")"
        choices = [("Selected", expr), ("Not selected", excluded)]
        if current:
            choices.extend(
                [
                    ("… and selected", combine(current, expr)),
                    ("… or selected", f"({current}) OR ({expr})"),
                    ("… and not selected", combine(current, excluded)),
                ]
            )
        for label, callback in (("Apply as Filter", self.apply), ("Prepare a Filter", self.prepare)):
            sub = menu.addMenu(label)
            for name, query in choices:
                action = sub.addAction(name, lambda checked=False, q=query, run=callback: run(q))
                action.setStatusTip(query)
        menu.addAction("Find matching events across case", lambda: self.apply_in(0, expr))
        menu.addAction("Copy BQL expression", lambda: copy(expr))

    def prepare(self, expr):
        self.entry.setText(expr)
        self.entry.setFocus()

    def copy_event(self):
        rows = self.selected_rows()
        if rows:
            copy(
                "\n".join(
                    "\t".join(
                        str(r[k])
                        for k in (
                            "timeline_id",
                            "time",
                            "source",
                            "name",
                            "sheet",
                            "rownum",
                            "actor",
                            "ip",
                            "op",
                            "result",
                            "verdict",
                        )
                    )
                    for r in rows
                )
            )

    def mark_events(self, verdict):
        if self.active is None or self.busy():
            return
        selected = self.table.selectionModel().selectedRows()
        if selected:
            self.remember_view()
            store.assign_verdict(
                self.db, "event", ((self.model.ids[i.row()], "") for i in selected), verdict
            )
            self.refresh()

    def mark_ip(self, verdict):
        if self.active is None or self.busy():
            return
        ips = sorted({ip for r in self.selected_rows() for ip in r["ip"].split(", ") if ip})
        if not ips:
            QMessageBox.information(self, APP, "Select an event with an IP address.")
            return
        if (
            QMessageBox.question(
                self,
                "Mark selected IPs",
                f"Apply {verdict} to all matching events in this case?\n\n"
                + "\n".join(ips[:20])
                + (f"\n… and {len(ips)-20} more" if len(ips) > 20 else ""),
            )
            == QMessageBox.StandardButton.Yes
        ):
            self.remember_view()
            store.assign_verdict(self.db, "ip", ((ip, "") for ip in ips), verdict)
            self.refresh()

    def mark_time(self, verdict):
        if self.active is None or self.busy():
            return
        times = sorted(r["time"] for r in self.selected_rows() if r["time"])
        dialog = QDialog(self)
        dialog.setWindowTitle("Mark UTC time range · " + verdict)
        form = QFormLayout(dialog)
        start = QLineEdit(times[0] if times else "")
        end = QLineEdit(
            utc(datetime.fromisoformat(times[-1]) + timedelta(microseconds=1))
            if times and not times[-1].startswith("9999-12-31")
            else ""
        )
        form.addRow("From UTC (inclusive)", start)
        form.addRow("Until UTC (exclusive)", end)
        message = QLabel()
        form.addRow(message)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )

        def accept():
            lo, hi = utc(start.text()), utc(end.text())
            if not lo or not hi or hi <= lo:
                message.setText("Enter an end time after the start.")
                return
            store.assign_verdict(self.db, "time", [(lo, hi)], verdict)
            dialog.accept()
            self.remember_view()
            self.refresh()

        buttons.accepted.connect(accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow(buttons)
        dialog.exec()

    def undo(self):
        if not self.db or self.busy():
            return
        store.undo_verdict(self.db)
        if self.active is not None:
            self.remember_view()
            self.refresh()

    def open_stats(self, key):
        if self.active is None:
            return
        label = self.log_name(self.logs[self.active]) if self.active else "All case events"
        dlg = Statistics(self, self.path, self.active, self.views[self.active].query, label, key)
        self.stats.append(dlg)
        dlg.finished.connect(lambda result, w=dlg: self.stats.remove(w) if w in self.stats else None)
        dlg.applyRequested.connect(self.apply_in)
        dlg.show()

    def import_details(self):
        if not self.db:
            return
        dlg = QDialog(self)
        dlg.setWindowTitle("Evidence and import details")
        dlg.resize(1060, 390)
        layout = QVBoxLayout(dlg)
        t = tree(
            [
                "Source file / sheet",
                "Log type",
                "Records",
                "Row issues",
                "Linked steps",
                "Unmatched steps",
                "Export limit",
            ]
        )
        t.setRootIsDecorated(False)
        t.setSortingEnabled(False)
        layout.addWidget(t)
        msg = QLabel("Checking imported files and authentication companions…")
        layout.addWidget(msg)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(dlg.reject)
        layout.addWidget(buttons)
        done = queue.Queue()
        stop = threading.Event()
        path = self.path

        def run():
            try:
                with contextlib.closing(store.read_case(path)) as db:
                    db.set_progress_handler(lambda: int(stop.is_set()), 1000)
                    done.put((quality(db), None))
            except Exception as exc:
                done.put(([], str(exc)))

        def finished():
            if done.empty():
                return
            rows, err = done.get()
            timer.stop()
            for _, name, sheet, kind, n, issues, warn, linked, missing in rows:
                node = QTreeWidgetItem(
                    t,
                    [
                        name + (" / " + sheet if sheet else ""),
                        kind,
                        f"{n:,}",
                        str(issues),
                        f"{linked:,}" if linked is not None else "—",
                        f"{missing:,}" if missing is not None else "—",
                        warn or "—",
                    ],
                )
                node.setToolTip(0, name)
                node.setToolTip(6, warn)
            for i in (1, 2, 3, 4, 5):
                t.resizeColumnToContents(i)
            msg.setText(err or f"{len(rows)} log(s) · {sum(r[4] for r in rows):,} source records")

        timer = QTimer(dlg)
        timer.timeout.connect(finished)
        timer.start(80)
        threading.Thread(target=run, daemon=True).start()
        try:
            dlg.exec()
        finally:
            stop.set()
            timer.stop()
            dlg.deleteLater()

    def help(self):
        dlg = QDialog(self)
        dlg.setWindowTitle("BQL display filters")
        dlg.resize(700, 490)
        layout = QVBoxLayout(dlg)
        notes = QLabel(
            'Compare fields using =, !=, contains, <, <=, > or >=. Combine with AND, OR, NOT and parentheses. Time comparisons use UTC. Use double quotes around text. For nested fields, try field "/AuditData/Parameters/*/Name" = "MoveToFolder".'
        )
        notes.setWordWrap(True)
        layout.addWidget(notes)
        fields = QLabel(
            "Available fields    "
            + "  ·  ".join(sorted(set(FIELDS) | set(SLOTS) | {"id", "row", "ip", "mailbox", "marked"}))
        )
        fields.setWordWrap(True)
        fields.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(fields)
        examples = tree(["Investigation", "Display filter"])
        examples.setRootIsDecorated(False)
        for label, expr in EXAMPLES:
            QTreeWidgetItem(examples, [label, expr])
        layout.addWidget(examples)
        controls = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        use = controls.addButton("Use selected filter", QDialogButtonBox.ButtonRole.ActionRole)

        def choose():
            item = examples.currentItem()
            if item:
                dlg.accept()
                self.apply(item.text(1))

        use.clicked.connect(choose)
        examples.itemDoubleClicked.connect(lambda item, col: choose())
        controls.rejected.connect(dlg.reject)
        layout.addWidget(controls)
        dlg.exec()

    def run_file_job(self, task):
        path = self.path

        def run():
            try:
                with contextlib.closing(store.connect(path)) as db:
                    kind, result = task(db)
                self.jobs.put((kind, None, result))
            except Exception as exc:
                self.jobs.put(("error", None, str(exc)))

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def import_paths(self, paths):
        if not self.db or self.busy():
            return

        def task(db):
            failed = []
            skipped = []
            for path in paths:
                self.jobs.put(("progress", None, "Opening " + Path(path).name))
                try:
                    state = store.import_file(db, path, lambda s: self.jobs.put(("progress", None, s)))
                    if state == "Already imported":
                        skipped.append(Path(path).name)
                except Exception as exc:
                    failed.append(f"{Path(path).name} · {exc}")
            return "import", (failed, skipped)

        self.run_file_job(task)

    def import_finished(self, result):
        old = set(self.logs)
        self.refresh_logs()
        added = [k for k in self.logs if k not in old]
        if self.logs:
            self.make_tab(0)
        for key in added:
            self.make_tab(key)
        if added:
            self.tab(added[0] if len(added) == 1 else 0)
        warnings = [
            self.logs[k]["name"] + " — " + self.logs[k]["warning"] for k in added if self.logs[k]["warning"]
        ]
        if warnings:
            QMessageBox.warning(
                self,
                "Possible export truncation",
                "\n".join(warnings) + "\n\nCollect smaller UTC windows or use reporting APIs.",
            )
        failed, skipped = result
        if failed:
            QMessageBox.warning(self, "Files not imported", "\n".join(failed))
        if skipped and not added:
            self.statusBar().showMessage(f"{len(skipped)} unchanged file(s) already in this case")
        self.update_actions()

    def extract_original(self):
        if self.active is None or self.busy():
            return
        key = self.active
        row = self.current()
        if not key and row:
            key = self.db.execute("SELECT log FROM events WHERE id=?", (row["id"],)).fetchone()[0]
        if not key:
            QMessageBox.information(self, APP, "Select a log tab or an event.")
            return
        log = self.logs[key]
        dest, _ = QFileDialog.getSaveFileName(self, "Save original evidence copy", "copy-" + log["name"])
        if dest:

            def task(db):
                store.extract(db, log["file"], dest)
                return "saved", dest

            self.run_file_job(task)

    def export_view(self):
        if self.active is None or self.busy():
            return
        dest, fmt = QFileDialog.getSaveFileName(
            self, "Export displayed events", "", "CSV (*.csv);;Excel workbook (*.xlsx)"
        )
        if not dest:
            return
        if not Path(dest).suffix:
            dest += ".xlsx" if "xlsx" in fmt else ".csv"
        log = self.active
        v = self.views[log]
        query = v.query
        desc = v.desc
        order = v.order
        custom = tuple(self.model.custom)

        def task(db):
            store.export(db, dest, log, query, desc, order_by=order, custom=custom)
            return "saved", dest

        self.statusBar().showMessage("Exporting displayed events…")
        self.run_file_job(task)

    def closeEvent(self, event: QCloseEvent):
        if self.busy():
            event.ignore()
            return
        self.cancel_query()
        for dlg in list(self.stats):
            dlg.close()
        self.settings.setValue("geometry", self.saveGeometry())
        self.settings.setValue("mainSplit", self.split.saveState())
        self.settings.setValue("detailSplit", self.inspector.saveState())
        self.settings.setValue("verdictColors", self.model.colors)
        self.settings.setValue("inspectorVertical", self.layout_action.isChecked())
        for key, action in self.pane_actions.items():
            self.settings.setValue(key + "PaneVisible", action.isChecked())
        self.timer.stop()
        self.model.reset(None, [])
        if self.db:
            self.db.close()
            self.db = None
        event.accept()


def main():
    parser = argparse.ArgumentParser(description=APP)
    parser.add_argument("--case", type=Path)
    parser.add_argument("--filter", help="Initial BQL display filter")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationDisplayName(APP)
    app.setApplicationName(APP)
    app.setWindowIcon(QIcon(str(Path(__file__).resolve().parent / "assets" / "classic-envelope.ico")))
    style(app)
    window = Analyzer()
    window.show()
    if args.case:
        window.load(args.case, new=not args.case.exists())
    if args.filter and window.db:
        window.apply(args.filter)
    if args.files and (window.db or window.new_case()):
        window.import_paths(args.files)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
