"""Desktop styling and timeline widgets."""

from collections import OrderedDict
from PySide6.QtCore import QAbstractTableModel, QEvent, QModelIndex, QObject, QRectF, QSize, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPalette, QPen, QPixmap
from PySide6.QtWidgets import QStyle, QStyledItemDelegate, QStyleOptionViewItem, QTabBar, QToolButton

import case_store as store

APP = "M365 Email Log Analyzer"
VERDICT_COLORS = {"Good": "#39FF14", "Bad": "#FF2020", "Unsure": "#FFFF00"}
COLS = ("timeline_id", "time", "source", "name", "actor", "ip", "op", "result")
LABELS = (
    "ID",
    "Time (UTC)",
    "Source",
    "Source file",
    "Actor / account",
    "IP address",
    "Operation",
    "Result",
)
# Minimum widths and stretch weights.
TIMELINE_SIZES = dict(zip(LABELS, (
    (62, 1), (154, 4), (108, 7), (111, 10), (124, 10),
    (104, 6), (110, 9), (72, 3),
), strict=True))


class ColumnLayout(QObject):
    """Size columns within available space."""

    def __init__(self, view, sizes, fallback=(108, 7)):
        super().__init__(view)
        self.view = view
        self.header = view.horizontalHeader() if hasattr(view, "horizontalHeader") else view.header()
        self.header_viewport = self.header.viewport()
        self.sizes = sizes
        self.fallback = fallback
        self.manual = {}
        self.fitting = False
        self.dragging_header = False
        self.header.setStretchLastSection(False)
        self.timer = QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.timeout.connect(self.fit)
        self.header.sectionResized.connect(self.section_resized)
        self.header.sectionCountChanged.connect(self.schedule)
        self.header.geometriesChanged.connect(self.schedule)
        view.model().modelReset.connect(self.schedule)
        view.viewport().installEventFilter(self)
        self.header_viewport.installEventFilter(self)
        self.schedule()

    def schedule(self, *_):
        if not self.fitting and not self.timer.isActive():
            self.timer.start(0)

    def eventFilter(self, widget, event):
        kind = event.type()
        if widget is self.header_viewport and kind == QEvent.Type.MouseButtonPress:
            self.dragging_header = True
        elif widget is self.header_viewport and kind in (QEvent.Type.MouseButtonRelease, QEvent.Type.Hide):
            self.dragging_header = False
            self.schedule()
        elif kind in (QEvent.Type.Resize, QEvent.Type.Show):
            self.schedule()
        return False

    def column_key(self, index):
        model = self.view.model()
        name = model.headerData(index, Qt.Orientation.Horizontal, Qt.ItemDataRole.DisplayRole)
        # Paths distinguish duplicate headings.
        path = model.headerData(index, Qt.Orientation.Horizontal, Qt.ItemDataRole.ToolTipRole)
        return name, path

    def section_resized(self, index, old, new):
        if self.fitting:
            return
        if old > 0 and new > 0 and index < self.header.count():
            self.manual[self.column_key(index)] = new
        self.schedule()

    def reset(self):
        self.manual.clear()
        self.fit()

    def fit(self):
        available = self.view.viewport().width()
        if self.dragging_header or available <= 0 or not self.view.isVisible():
            return
        sections = []
        for index in range(self.header.count()):
            if self.header.isSectionHidden(index):
                continue
            key = self.column_key(index)
            minimum, weight = self.sizes.get(key[0], self.fallback)
            width = max(minimum, self.header.minimumSectionSize(), self.manual.get(key, 0))
            sections.append((index, width, weight, key in self.manual))
        if not sections:
            return
        extra = max(0, available - sum(width for _, width, _, _ in sections))
        all_manual = all(manual for _, _, _, manual in sections)
        weight_left = sum(weight for _, _, weight, manual in sections if all_manual or not manual)
        self.fitting = True
        try:
            for index, width, weight, manual in sections:
                if all_manual or not manual:
                    share = extra * weight // weight_left
                    width += share
                    extra -= share
                    weight_left -= weight
                if self.header.sectionSize(index) != width:
                    self.header.resizeSection(index, width)
        finally:
            self.fitting = False


def style(app):
    app.setStyle("Fusion")
    app.setFont(QFont("Segoe UI", 9))
    palette = QPalette()
    for role, color in {
        "Window": "#2c2c2c",
        "WindowText": "#ededed",
        "Base": "#262829",
        "AlternateBase": "#2c2e2f",
        "Text": "#ededed",
        "Button": "#343638",
        "ButtonText": "#e4e7e8",
        "Highlight": "#78508b",
        "HighlightedText": "#ffffff",
        "ToolTipBase": "#383c3e",
        "ToolTipText": "#ffffff",
        "PlaceholderText": "#a5aaaa",
        "Link": "#a3c8ea",
        "Light": "#55585a",
        "Mid": "#4d5052",
        "Dark": "#202122",
        "BrightText": "#ffffff",
    }.items():
        palette.setColor(getattr(QPalette.ColorRole, role), QColor(color))
    palette.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.Text, QColor("#94989a"))
    palette.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.ButtonText, QColor("#86898b"))
    app.setPalette(palette)
    app.setStyleSheet("""
    QMainWindow,QDialog {background:#2c2c2c}
    QMenuBar,QToolBar,QStatusBar {background:#202122}
    QMenuBar {border-bottom:1px solid #525455;padding:1px}
    QMenuBar::item {padding:3px 7px} QMenuBar::item:selected,QMenu::item:selected {background:#485362}
    QMenu {background:#303233;border:1px solid #575b5d;padding:3px}
    QToolBar {border:0;border-bottom:1px solid #55585b;spacing:1px;padding:2px}
    QToolBar::separator {width:1px;background:#535657;margin:4px}
    QToolButton {padding:3px;border:1px solid transparent;border-radius:2px}
    QToolButton:hover {background:#404548;border-color:#646a6c}
    QToolButton:pressed {background:#50545a}
    QFrame#filterBar {background:#222324;border-bottom:1px solid #55585a}
    QFrame#paneBar {background:#333536;border-bottom:1px solid #505355}
    QLabel#secondary {color:#b8bfc1}
    QLineEdit,QPlainTextEdit,QComboBox {background:#252728;selection-background-color:#6b537b}
    QLineEdit {border:1px solid #5c6062;border-radius:2px;padding:3px 6px}
    QLineEdit:focus {border-color:#8bacbf}
    QLineEdit[validity="valid"] {background:#273a30;border-color:#719683}
    QLineEdit[validity="error"] {background:#442d30;border-color:#bc7c79}
    QComboBox {padding:2px 5px;border:1px solid #626568;border-radius:2px;min-height:18px}
    QTabBar::tab {background:#252728;border:0;border-top:2px solid transparent;padding:3px 7px;margin-right:1px}
    QTabBar::tab:hover {background:#303334}
    QTabBar::tab:selected {background:#383b3d;border-top:2px solid #a4c9e9}
    QToolButton#closeButton {padding:0;border:0;background:transparent}
    QToolButton#closeButton:hover,QToolButton#closeButton:focus {background:#575b5e}
    QToolButton#closeButton:pressed {background:#694449}
    QHeaderView::section {background:#353739;border:none;border-right:1px solid #55585a;
                          border-bottom:1px solid #545758;padding:3px 6px;color:#e3e6e7}
    QTreeView,QTableView,QPlainTextEdit {border:0}
    QTreeView::item,QTableView::item {padding:1px 4px}
    QTreeView::item:selected,QTableView::item:selected {background:#754086;color:white}
    QTableView#timeline::item:selected {background:#474b4e;color:white}
    QSplitter::handle {background:#535657} QSplitter::handle:hover {background:#829eac}
    QStatusBar {border-top:1px solid #515457} QStatusBar::item {border:0}
    QToolTip {background:#383b3d;color:#eee;border:1px solid #686d70;padding:3px}
    """)


def icon(code, color="#bec4c7"):
    px = QPixmap(40, 40)
    px.fill(Qt.GlobalColor.transparent)
    p = QPainter(px)
    p.setRenderHint(QPainter.RenderHint.TextAntialiasing)
    font = QFont("Segoe MDL2 Assets")
    font.setPixelSize(29)
    p.setFont(font)
    p.setPen(QColor(color))
    p.drawText(QRectF(0, 0, 40, 40), Qt.AlignmentFlag.AlignCenter, chr(code))
    p.end()
    return QIcon(px)


def close_button(label):
    button = QToolButton()
    button.setObjectName("closeButton")
    button.setToolTip(label)
    button.setAccessibleName(label)
    button.setFixedSize(22, 22)
    pixmap = QPixmap(24, 24)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(QPen(QColor("#e4e7e8"), 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
    painter.drawLine(5, 5, 19, 19)
    painter.drawLine(19, 5, 5, 19)
    painter.end()
    button.setIcon(QIcon(pixmap))
    button.setIconSize(QSize(14, 14))
    return button


class LogTabs(QTabBar):
    def __init__(self):
        super().__init__()
        self.keyboard_focus = False

    def focusInEvent(self, event):
        self.keyboard_focus = event.reason() in (
            Qt.FocusReason.TabFocusReason,
            Qt.FocusReason.BacktabFocusReason,
            Qt.FocusReason.ShortcutFocusReason,
        )
        super().focusInEvent(event)

    def mousePressEvent(self, event):
        self.keyboard_focus = False
        super().mousePressEvent(event)
        self.update()

    def keyPressEvent(self, event):
        self.keyboard_focus = True
        super().keyPressEvent(event)
        self.update()

    def initStyleOption(self, option, index):
        super().initStyleOption(option, index)
        if self.hasFocus() and self.keyboard_focus and self.currentIndex() == index:
            option.state |= QStyle.StateFlag.State_HasFocus | QStyle.StateFlag.State_KeyboardFocusChange
        else:
            option.state &= ~QStyle.StateFlag.State_HasFocus

    def add_close_button(self, index, label):
        button = close_button(label)
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        def close_tab():
            for i in range(self.count()):
                if self.tabButton(i, QTabBar.ButtonPosition.RightSide) is button:
                    self.tabCloseRequested.emit(i)
                    break

        button.clicked.connect(close_tab)
        self.setTabButton(index, QTabBar.ButtonPosition.RightSide, button)


class VerdictDelegate(QStyledItemDelegate):
    def paint(self, painter, option, index):
        if option.state & QStyle.StateFlag.State_Selected and index.data(Qt.ItemDataRole.BackgroundRole):
            option = QStyleOptionViewItem(option)
            option.state &= ~QStyle.StateFlag.State_Selected
            super().paint(painter, option, index)
            painter.save()
            painter.setPen(QColor("#171717"))
            painter.drawLine(option.rect.topLeft(), option.rect.topRight())
            painter.drawLine(option.rect.bottomLeft(), option.rect.bottomRight())
            painter.restore()
        else:
            super().paint(painter, option, index)


class EventTable(QAbstractTableModel):
    """Cache visible timeline pages."""

    page_size = 160

    def __init__(self):
        super().__init__()
        self.db = None
        self.ids = []
        self.pages = OrderedDict()
        self.colors = True
        self.custom = []
        self.font = QFont("Consolas", 9)

    def append_field(self, label, path):
        if any(p == path for _, p in self.custom):
            return False
        n = self.columnCount()
        self.beginInsertColumns(QModelIndex(), n, n)
        self.custom.append((label, path))
        self.pages.clear()
        self.endInsertColumns()
        return True

    def remove_field(self, n):
        self.beginRemoveColumns(QModelIndex(), n, n)
        self.custom.pop(n - len(COLS))
        self.pages.clear()
        self.endRemoveColumns()

    def reset(self, db, ids):
        self.beginResetModel()
        self.db = db
        self.ids = ids
        self.pages.clear()
        self.endResetModel()

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.ids)

    def columnCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(COLS) + len(self.custom)

    def headerData(self, index, orient, role):
        if orient == Qt.Orientation.Horizontal:
            if role == Qt.ItemDataRole.DisplayRole:
                return LABELS[index] if index < len(COLS) else self.custom[index - len(COLS)][0]
            if role == Qt.ItemDataRole.ToolTipRole and index >= len(COLS):
                return self.custom[index - len(COLS)][1]

    def record(self, row):
        if not 0 <= row < len(self.ids):
            return
        page = row // self.page_size
        if page not in self.pages:
            part = self.ids[page * self.page_size : (page + 1) * self.page_size]
            sql, args = store.select_sql(self.custom)
            sql += " WHERE e.id IN (" + ",".join("?" for _ in part) + ")"
            self.pages[page] = {r["id"]: r for r in self.db.execute(sql, args + list(part))}
            if len(self.pages) > 10:
                self.pages.popitem(last=False)
        self.pages.move_to_end(page)
        return self.pages[page].get(self.ids[row])

    def data(self, index, role):
        if not index.isValid():
            return
        if role not in (
            Qt.ItemDataRole.DisplayRole,
            Qt.ItemDataRole.BackgroundRole,
            Qt.ItemDataRole.ForegroundRole,
            Qt.ItemDataRole.FontRole,
            Qt.ItemDataRole.ToolTipRole,
            Qt.ItemDataRole.TextAlignmentRole,
        ):
            return
        col = COLS[index.column()] if index.column() < len(COLS) else f"custom_{index.column()-len(COLS)}"
        if role == Qt.ItemDataRole.FontRole:
            return self.font if col in ("timeline_id", "time", "ip") else None
        if role == Qt.ItemDataRole.TextAlignmentRole:
            return (
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                if col == "timeline_id"
                else None
            )
        row = self.record(index.row())
        if row is None:
            return
        color = VERDICT_COLORS.get(row["verdict"]) if self.colors else None
        if role == Qt.ItemDataRole.BackgroundRole:
            return QColor(color) if color else None
        if role == Qt.ItemDataRole.ForegroundRole:
            if color:
                return QColor("#101010")
            if col == "timeline_id":
                return QColor("#afbbbf")
            return
        val = row[col]
        if role == Qt.ItemDataRole.DisplayRole and col == "time" and val:
            return val.replace("T", " ")[:19] + (val[19:-1].rstrip("0").rstrip(".") if "." in val else "")
        return str(val)
