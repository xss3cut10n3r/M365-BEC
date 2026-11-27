"""Explore filtered event statistics."""

import contextlib
import csv
from pathlib import Path
import queue
import threading
from PySide6.QtCore import (
    QAbstractTableModel,
    QModelIndex,
    QRectF,
    QSortFilterProxyModel,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QMessageBox,
    QPushButton,
    QTableView,
    QToolTip,
    QVBoxLayout,
    QWidget,
)

import case_store as store
from desktop_style import ColumnLayout
from record_view import copy
from telemetry import REPORTS, calculate, combine, drilldown, utc_text


class Counts(QAbstractTableModel):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.headers = []
        self.activity = False
        self.total = 0

    def replace(self, rows, headings, activity, total=0):
        self.beginResetModel()
        self.rows = rows
        self.headings = headings
        self.total = total
        self.headers = list(headings) + ["Events", "% displayed", "First seen (UTC)", "Last seen (UTC)"]
        self.activity = activity
        self.endResetModel()

    def values(self, row):
        n = len(self.headings)
        return (*row[: n + 1], 100 * row[n] / self.total if self.total else 0, *row[n + 1 :])

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.headers)

    def headerData(self, index, orient, role):
        if role == Qt.ItemDataRole.DisplayRole and orient == Qt.Orientation.Horizontal:
            return self.headers[index]

    def data(self, index, role):
        if not index.isValid():
            return
        col = index.column()
        value = self.values(self.rows[index.row()])[col]
        if role == Qt.ItemDataRole.UserRole:
            return value
        if role == Qt.ItemDataRole.TextAlignmentRole and col in (
            len(self.headings),
            len(self.headings) + 1,
        ):
            return Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight
        if role in (Qt.ItemDataRole.DisplayRole, Qt.ItemDataRole.ToolTipRole):
            if self.activity and col == 0:
                return utc_text(value).replace("T", " ")
            if col == len(self.headings) + 1:
                return f"{value:.2f}%"
            return f"{value:,}" if isinstance(value, int) else value


class ActivityPlot(QWidget):
    intervalSelected = Signal(object)

    def __init__(self):
        super().__init__()
        self.setMinimumHeight(145)
        self.setMaximumHeight(230)
        self.setMouseTracking(True)
        self.rows = []
        self.step = 3600
        self.axis = None
        self.setAccessibleName("Event counts by UTC time interval")

    def set_rows(self, rows, step):
        self.rows = rows
        self.step = step
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor("#242729"))
        x0, y0 = 57, 22
        w = max(1, self.width() - x0 - 20)
        h = max(1, self.height() - y0 - 28)
        p.setPen(QColor("#c2cbcc"))
        p.drawText(8, 15, "Events")
        if not self.rows:
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "No events in this view")
            return
        start = self.rows[0][0]
        end = self.rows[-1][0] + self.step
        peak = max(r[1] for r in self.rows)
        for fraction in (0, 0.5, 1):
            y = y0 + int(h * (1 - fraction))
            p.setPen(QColor("#42494b"))
            p.drawLine(x0, y, x0 + w, y)
            p.setPen(QColor("#b7c3c4"))
            p.drawText(
                QRectF(1, y - 9, x0 - 7, 18),
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                f"{int(peak*fraction):,}",
            )
        for t, n, _, _ in self.rows:
            x = x0 + int((t - start) * w / (end - start))
            width = max(1, self.step * w / (end - start) - 1)
            height = max(1, n * h / peak)
            rect = QRectF(x, y0 + h - height, width, height)
            p.fillRect(rect, QColor("#8bbbd2"))
            # Resolve overlapping interval bars.
        self.axis = (x0, w, start, end)
        first = utc_text(start)[:16].replace("T", " ")
        last = utc_text(end)[:16].replace("T", " ")
        p.setPen(QColor("#c2cbcc"))
        p.drawText(x0, self.height() - 7, first)
        p.drawText(
            QRectF(x0, self.height() - 24, w, 20),
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
            last + " UTC",
        )

    def bucket(self, event):
        if not self.rows or self.axis is None:
            return
        x0, w, start, end = self.axis
        if x0 <= event.position().x() < x0 + w:
            return start + int((event.position().x() - x0) / w * (end - start) / self.step) * self.step

    def mouseMoveEvent(self, event):
        t = self.bucket(event)
        if t is not None:
            from bisect import bisect_left

            i = bisect_left(self.rows, t, key=lambda r: r[0])
            n = self.rows[i][1] if i < len(self.rows) and self.rows[i][0] == t else 0
            QToolTip.showText(event.globalPosition().toPoint(), f"{utc_text(t)} · {n:,} events", self)

    def mouseDoubleClickEvent(self, event):
        t = self.bucket(event)
        if t is not None:
            self.intervalSelected.emit(t)


class Statistics(QDialog):
    applyRequested = Signal(object, str)

    def __init__(self, parent, path, log, display_filter, scope, key):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.setWindowTitle("Statistics")
        self.resize(910, 570)
        self.path = path
        self.original_log = log
        self.original_filter = display_filter
        self.log = log
        self.base = display_filter
        self.generation = 0
        self.cancel = threading.Event()
        self.results = queue.Queue()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 10)
        layout.setSpacing(7)
        controls = QHBoxLayout()
        self.kind = QComboBox()
        for k, r in REPORTS.items():
            self.kind.addItem(r.title, k)
        self.kind.setCurrentIndex(self.kind.findData(key))
        self.kind.setAccessibleName("Statistics report")
        controls.addWidget(self.kind)
        self.interval = QComboBox()
        for label, n in [("1 minute", 60), ("15 minutes", 900), ("1 hour", 3600), ("1 day", 86400)]:
            self.interval.addItem(label, n)
        self.interval.setCurrentIndex(2)
        self.interval.setAccessibleName("Time interval")
        controls.addWidget(self.interval)
        controls.addStretch()
        self.search = QLineEdit()
        self.search.setPlaceholderText("Find a group…")
        self.search.setClearButtonEnabled(True)
        controls.addWidget(self.search)
        layout.addLayout(controls)
        scope_row = QHBoxLayout()
        self.scope = QComboBox()
        self.scope.setAccessibleName("Log scope")
        if log:
            self.scope.addItem(scope, log)
        self.scope.addItem("All case events", 0)
        scope_row.addWidget(self.scope)
        self.limit = QCheckBox("Limit to display filter")
        self.limit.setChecked(bool(display_filter))
        self.limit.setEnabled(bool(display_filter))
        self.limit.setToolTip(display_filter or "No display filter applied")
        scope_row.addWidget(self.limit)
        self.filter_label = QLabel()
        self.filter_label.setTextFormat(Qt.TextFormat.PlainText)
        self.filter_label.setObjectName("secondary")
        self.filter_label.setWordWrap(True)
        scope_row.addWidget(self.filter_label, 1)
        layout.addLayout(scope_row)
        self.graph = ActivityPlot()
        layout.addWidget(self.graph)
        self.model = Counts()
        self.proxy = QSortFilterProxyModel(self)
        self.proxy.setSourceModel(self.model)
        self.proxy.setFilterKeyColumn(-1)
        self.proxy.setSortRole(Qt.ItemDataRole.UserRole)
        self.proxy.setFilterCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.table = QTableView()
        self.table.setModel(self.proxy)
        self.table.setSortingEnabled(True)
        self.table.setAlternatingRowColors(True)
        self.table.setShowGrid(False)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(21)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setHorizontalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.column_layout = ColumnLayout(self.table, {
            "Events": (68, 1), "% displayed": (93, 1),
            "First seen (UTC)": (146, 4), "Last seen (UTC)": (146, 4),
        }, fallback=(120, 7))
        self.table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.table.customContextMenuRequested.connect(self.context)
        layout.addWidget(self.table, 1)
        foot = QHBoxLayout()
        self.message = QLabel()
        foot.addWidget(self.message, 1)
        self.apply_button = QPushButton("Apply as filter")
        self.apply_button.clicked.connect(self.apply_selected)
        foot.addWidget(self.apply_button)
        save = QPushButton("Save CSV…")
        save.clicked.connect(self.export)
        foot.addWidget(save)
        close = QPushButton("Close")
        close.clicked.connect(self.close)
        foot.addWidget(close)
        layout.addLayout(foot)
        self.search.textChanged.connect(self.proxy.setFilterFixedString)
        self.table.doubleClicked.connect(self.apply_selected)
        self.graph.intervalSelected.connect(lambda t: self.apply_values([t]))
        self.kind.currentIndexChanged.connect(self.refresh)
        self.interval.currentIndexChanged.connect(self.refresh)
        self.scope.currentIndexChanged.connect(self.refresh)
        self.limit.toggled.connect(self.refresh)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(60)
        self.refresh()

    def refresh(self, *args):
        self.cancel.set()
        self.cancel = threading.Event()
        self.generation += 1
        token = self.generation
        cancel = self.cancel
        key = self.kind.currentData()
        step = self.interval.currentData()
        report = REPORTS[key]
        self.log = log = self.scope.currentData()
        self.base = base = self.original_filter if self.limit.isChecked() else ""
        self.filter_label.setText(base)
        self.interval.setVisible(key == "activity")
        self.graph.setVisible(key == "activity")
        self.model.replace([], report.headings, key == "activity")
        self.graph.set_rows([], step)
        self.message.setText("Calculating across matching events…")
        self.apply_button.setEnabled(False)

        def run():
            try:
                with contextlib.closing(store.read_case(self.path)) as db:
                    db.set_progress_handler(lambda: int(cancel.is_set()), 1000)
                    db.execute("BEGIN")
                    total = store.count_rows(db, log, base)
                    rows = calculate(db, key, log, base, step)
                if not cancel.is_set():
                    self.results.put((token, key, rows, total, None))
            except Exception as exc:
                if not cancel.is_set():
                    self.results.put((token, key, [], 0, str(exc)))

        threading.Thread(target=run, daemon=True).start()

    def poll(self):
        while not self.results.empty():
            token, key, rows, total, err = self.results.get()
            if token != self.generation:
                continue
            report = REPORTS[key]
            self.model.replace(rows, report.headings, key == "activity", total)
            self.graph.set_rows(rows if key == "activity" else [], self.interval.currentData())
            self.table.sortByColumn(
                0 if key == "activity" else len(report.fields),
                Qt.SortOrder.AscendingOrder if key == "activity" else Qt.SortOrder.DescendingOrder,
            )
            self.message.setText(err or f"{len(rows):,} groups · {total:,} displayed events")
            self.message.setToolTip(
                "Each event contributes once per distinct matching group. Groups can overlap."
            )
            self.apply_button.setEnabled(bool(rows))
            if rows:
                self.table.selectRow(0)

    def selected(self):
        idx = self.proxy.mapToSource(self.table.currentIndex())
        return self.model.rows[idx.row()] if idx.isValid() else None

    def apply_values(self, values):
        expr = drilldown(self.kind.currentData(), values, self.interval.currentData())
        self.applyRequested.emit(self.log, combine(self.base, expr))
        self.accept()

    def apply_selected(self, *_):
        row = self.selected()
        if row:
            self.apply_values(row)

    def context(self, pos):
        idx = self.table.indexAt(pos)
        if not idx.isValid():
            return
        self.table.selectRow(idx.row())
        menu = QMenu(self)
        menu.addAction("Apply as filter", self.apply_selected)
        menu.addAction("Copy selected group", lambda: copy("\t".join(map(str, self.selected()))))
        menu.exec(self.table.viewport().mapToGlobal(pos))

    def export(self):
        if not self.model.rows:
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Save statistics", self.kind.currentData() + ".csv", "CSV (*.csv)"
        )
        if not path:
            return
        from tempfile import NamedTemporaryFile
        import os

        temp = None
        try:
            with contextlib.closing(store.read_case(self.path)) as db:
                store.guard_destination(db, path)
            with NamedTemporaryFile(
                "w", newline="", encoding="utf-8-sig", dir=Path(path).parent, delete=False
            ) as f:
                temp = Path(f.name)
                w = csv.writer(f)
                w.writerow(self.model.headers)
                for i in range(self.proxy.rowCount()):
                    row = list(
                        self.model.values(
                            self.model.rows[self.proxy.mapToSource(self.proxy.index(i, 0)).row()]
                        )
                    )
                    if self.model.activity:
                        row[0] = utc_text(row[0])
                    w.writerow(
                        [
                            (
                                "'" + v
                                if isinstance(v, str) and v.lstrip().startswith(("=", "+", "-", "@"))
                                else v
                            )
                            for v in row
                        ]
                    )
            os.replace(temp, path)
        except Exception as exc:
            QMessageBox.warning(self, "Save statistics", str(exc))
        finally:
            if temp:
                temp.unlink(missing_ok=True)

    def done(self, result):
        self.cancel.set()
        super().done(result)
