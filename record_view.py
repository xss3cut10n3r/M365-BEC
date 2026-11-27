"""Inspect original event records."""

import csv
import io
import json
import re
from collections import Counter
from itertools import islice
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QFontDatabase, QTextCursor
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMenu,
    QPlainTextEdit,
    QSplitter,
    QStackedWidget,
    QToolButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)
from desktop_style import ColumnLayout, close_button
from event_fields import EventFields, SLOTS, decode, escape_pointer
from telemetry import quote

ROLE = Qt.ItemDataRole.UserRole
FILTER_ROLE = ROLE + 1
MONO = QFontDatabase.SystemFont.FixedFont


def compact_layout(widget, horizontal=False, margin=0):
    layout = (QHBoxLayout if horizontal else QVBoxLayout)(widget)
    layout.setContentsMargins(margin, margin, margin, margin)
    layout.setSpacing(4 if horizontal else 0)
    return layout


def tool(text, tip, slot):
    button = QToolButton()
    button.setText(text)
    button.setToolTip(tip)
    button.setAccessibleName(tip)
    button.clicked.connect(slot)
    return button


def copy(value):
    QApplication.clipboard().setText(str(value))


def tree(headers):
    view = QTreeWidget()
    view.setHeaderLabels(headers)
    view.setUniformRowHeights(True)
    view.setAlternatingRowColors(True)
    view.setIndentation(16)
    view.setHorizontalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
    view.header().setSectionResizeMode(QHeaderView.ResizeMode.Interactive)
    view.setColumnWidth(0, 225)
    return view


def show_value(value):
    if isinstance(value, dict):
        for key in ("Subject", "Path", "Name", "DisplayName", "Operation", "Address"):
            if isinstance(value.get(key), str) and value[key]:
                return value[key] + f"  ·  {len(value)} fields"
        return f"{len(value)} fields"
    if isinstance(value, list):
        return f"{len(value)} item" if len(value) == 1 else f"{len(value)} items"
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False)


def serialize(value):
    return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)


def source_predicate(pointer, value):
    if isinstance(value, (list, dict)):
        return f"exists {quote(pointer)}"
    return f"field {quote(pointer)} = {quote(show_value(value))}"


def evidence_row_predicate(row):
    return (
        f"path = {quote(row['path'])} AND sha256 = {quote(row['digest'])} "
        f"AND sheet = {quote(row['sheet'])} AND row = {row['rownum']}"
    )


def json_line_targets(value, pointer):
    """Map JSON lines to fields."""
    yield pointer, value
    if isinstance(value, (dict, list)) and value:
        children = value.items() if isinstance(value, dict) else (("*", v) for v in value)
        for key, child in children:
            path = pointer + "/" + escape_pointer(key) if pointer is not None else None
            yield from json_line_targets(child, path)
        yield pointer, value


def csv_cell_at(prefix):
    row = column = 0
    quoted = False
    for c in prefix:
        if c == '"':
            quoted = not quoted
        elif not quoted and c == ",":
            column += 1
        elif not quoted and c == "\n":
            row += 1
            column = 0
    return row, column


def sections(header, lookup):
    """Group columns by evidence type."""
    target = re.fullmatch(
        r"Target(\d+)(?:ModifiedProperty(\d+)(?:Name|OldValue|NewValue)|.*)", header, re.I
    )
    if target:
        n, prop = target.groups()
        prefix = ("Directory targets", "Target " + n)
        if prop:
            name = lookup.get(f"Target{n}ModifiedProperty{prop}Name")
            return prefix + (f"Property {prop}" + (" · " + name if name else ""),)
        return prefix
    extra = re.fullmatch(r"AdditionalDetail(\d+)(?:Key|Value)", header, re.I)
    if extra:
        n = extra.group(1)
        return ("Additional details", lookup.get("AdditionalDetail" + n + "Key") or "Detail " + n)
    h = header.casefold()
    if h.startswith("actor") or h in ("user", "userid", "user id", "username", "user type", "mailbox"):
        return ("Identity",)
    if any(
        s in h
        for s in (
            "ip address",
            "device",
            "operating system",
            "browser",
            "location",
            "autonomous system",
            "client app",
            "compliant",
            "managed",
            "join type",
            "user agent",
            "ipaddress",
        )
    ):
        return ("Device and network",)
    if any(
        s in h
        for s in (
            "auth",
            "conditional access",
            "status",
            "result",
            "succeeded",
            "failure",
            "credential",
            "requirement",
            "error code",
        )
    ):
        return ("Authentication and outcome",)
    if any(s in h for s in ("session", "token", "correlation", "request id")):
        return ("Session and tokens",)
    if any(s in h for s in ("app", "resource", "tenant", "service principal")):
        return ("Application and resource",)
    return ("Event fields",)


class Inspector(QSplitter):
    paneCloseRequested = Signal(str)
    filterMenuRequested = Signal(object, str)
    columnRequested = Signal(str, str)
    followEvent = Signal(int)

    def __init__(self):
        super().__init__(Qt.Orientation.Horizontal)
        self.setChildrenCollapsible(False)
        self.headers = []
        self.values = []
        self.sources = []
        self.pointers = []
        self.nodes = {}
        self.last_key = None
        self.hidden_empty = False
        self.expanded = set()
        self.rendering = False
        left = QWidget()
        ll = compact_layout(left)
        bar = QFrame()
        bar.setObjectName("paneBar")
        bh = compact_layout(bar, True, 4)
        bh.addWidget(QLabel("Event details"))
        bh.addStretch()
        bh.addWidget(tool("＋", "Expand event details", lambda: self.details.expandAll()))
        bh.addWidget(tool("−", "Collapse event details", lambda: self.details.collapseAll()))
        close_details = close_button("Close event details")
        close_details.clicked.connect(lambda: self.paneCloseRequested.emit("details"))
        bh.addWidget(close_details)
        ll.addWidget(bar)
        self.details = tree(["Field", "Value"])
        self.detail_columns = ColumnLayout(self.details, {"Field": (115, 4), "Value": (120, 6)})
        ll.addWidget(self.details)
        self.addWidget(left)
        right = QWidget()
        rl = compact_layout(right)
        bar = QFrame()
        bar.setObjectName("paneBar")
        bh = compact_layout(bar, True, 4)
        bh.addWidget(QLabel("Source record"))
        self.source = QComboBox()
        self.source.setAccessibleName("Record data source")
        self.source.setMinimumContentsLength(9)
        self.source.setMaximumWidth(245)
        self.source.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        bh.addWidget(self.source, 1)
        self.mode = QComboBox()
        self.mode.setAccessibleName("Record presentation")
        self.mode.addItems(["Field tree", "Formatted JSON", "Row / cell text"])
        self.mode.setMaximumWidth(160)
        bh.addWidget(self.mode)
        more = QToolButton()
        more.setText("⋯")
        more.setToolTip("Source record options")
        menu = QMenu(more)
        menu.addAction("Expand branches", lambda: self.expand_loaded())
        menu.addAction("Collapse branches", lambda: self.fields.collapseAll())
        empty = menu.addAction("Hide empty fields")
        empty.setCheckable(True)
        empty.toggled.connect(self.toggle_empty)
        wrap = menu.addAction("Wrap text")
        wrap.setCheckable(True)
        wrap.setChecked(True)
        wrap.toggled.connect(self.wrap)
        menu.addAction("Copy this data source", self.copy_source)
        more.setMenu(menu)
        more.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        bh.addWidget(more)
        close_source = close_button("Close source record")
        close_source.clicked.connect(lambda: self.paneCloseRequested.emit("source"))
        bh.addWidget(close_source)
        rl.addWidget(bar)
        self.pages = QStackedWidget()
        self.fields = tree(["Field", "Value"])
        self.source_columns = ColumnLayout(self.fields, {"Field": (115, 4), "Value": (120, 6)})
        self.fields.itemExpanded.connect(self.open_branch)
        self.fields.itemCollapsed.connect(self.close_branch)
        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.text.customContextMenuRequested.connect(self.text_context)
        self.text.setFont(QFontDatabase.systemFont(MONO))
        self.pages.addWidget(self.fields)
        self.pages.addWidget(self.text)
        rl.addWidget(self.pages, 1)
        findrow = QFrame()
        fl = compact_layout(findrow, True, 3)
        self.find = QLineEdit()
        self.find.setClearButtonEnabled(True)
        self.find.setPlaceholderText("Find in this record…")
        self.find.setAccessibleName("Find in this record")
        self.find.returnPressed.connect(self.find_next)
        fl.addWidget(self.find)
        self.find_status = QLabel()
        fl.addWidget(self.find_status)
        fl.addWidget(tool("Next", "Find next match", self.find_next))
        rl.addWidget(findrow)
        self.addWidget(right)
        self.setStretchFactor(0, 4)
        self.setStretchFactor(1, 6)
        self.mode.currentIndexChanged.connect(self.render)
        self.source.currentIndexChanged.connect(self.render)
        self.details.itemClicked.connect(self.select_detail)
        self.details.itemDoubleClicked.connect(self.follow)
        for view in (self.details, self.fields):
            view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            view.customContextMenuRequested.connect(lambda pos, v=view: self.context(v, pos))
        self.clear_record()

    def clear_record(self):
        self.last_key = None
        self.last_source = None
        self.headers = []
        self.values = []
        self.sources = []
        self.pointers = []
        self.details.clear()
        self.fields.clear()
        self.nodes.clear()
        self.source.clear()
        self.text.clear()
        self.find_status.clear()

    def wrap(self, on):
        self.text.setLineWrapMode(
            QPlainTextEdit.LineWrapMode.WidgetWidth if on else QPlainTextEdit.LineWrapMode.NoWrap
        )

    def toggle_empty(self, on):
        self.hidden_empty = on
        self.render()

    def set_record(self, db, key, source, reason, verdict="Unmarked", timeline_id=None):
        if self.last_key == (key, reason, verdict, timeline_id):
            return
        r = db.execute(
            """SELECT e.*,l.kind,l.headers,l.name,l.sheet,f.digest,f.path FROM events e JOIN logs l ON e.log=l.id
                        JOIN files f ON l.file=f.id WHERE e.id=?""",
            (key,),
        ).fetchone()
        if not r:
            self.clear_record()
            return
        preferred = self.source.currentText() if source == self.last_source else ""
        self.last_source = source
        open_groups = {
            self.details.topLevelItem(i).text(0)
            for i in range(self.details.topLevelItemCount())
            if self.details.topLevelItem(i).isExpanded()
        }
        first = self.last_key is None
        self.last_key = (key, reason, verdict, timeline_id)
        self.headers = json.loads(r["headers"])
        self.values = json.loads(r["raw"])
        normalized = [" ".join(h.casefold().split()) for h in self.headers]
        count = Counter(normalized)
        seen = Counter()
        self.sources = []
        self.pointers = []
        for i in range(max(len(self.headers), len(self.values))):
            h = self.headers[i] if i < len(self.headers) else f"Extra column {i+1}"
            norm = " ".join(h.casefold().split())
            seen[norm] += 1
            label = f"{h} [{seen[norm]}]" if count[norm] > 1 else h
            header_key = f"@{seen[norm]}:{h}" if count[norm] > 1 or re.match(r"^@[1-9]\d*:", h) else h
            self.pointers.append(
                "/" + escape_pointer(header_key) if i < min(len(self.headers), len(self.values)) else None
            )
            self.sources.append((label, self.values[i] if i < len(self.values) else None))
        self.source.blockSignals(True)
        self.source.clear()
        self.source.addItem("Imported row", -1)
        for i, (name, val) in enumerate(self.sources):
            self.source.addItem(name, i)
        idx = self.source.findText(preferred) if preferred else -1
        if idx < 0:
            idx = max(0, self.source.findText("AuditData"))
        self.source.setCurrentIndex(idx)
        self.source.blockSignals(False)
        self.details.clear()

        def group(name, summary="", field=None):
            node = QTreeWidgetItem(self.details, [name, summary])
            font = node.font(0)
            font.setBold(True)
            node.setFont(0, font)
            if field:
                node.setData(0, ROLE, (field, summary))
            node.setExpanded(name in open_groups or first and name == "Event")
            return node

        def leaf(parent, name, val, field=None):
            if val is None or val == "":
                return None
            val = str(val)
            node = QTreeWidgetItem(parent, [name, val])
            node.setToolTip(1, val)
            node.setData(0, ROLE, (field, val))
            return node

        event = group("Event", r["op"], "operation")
        for name, value, field in [
            ("ID", timeline_id, "id"),
            ("Time (UTC)", r["time"], "time"),
            ("Actor", r["actor"], "actor"),
            ("Operation", r["op"], "operation"),
            ("Result", r["result"], "result"),
            ("Source", source, "source"),
            ("Verdict", verdict, "verdict"),
        ]:
            leaf(event, name, value, field)
        if reason:
            leaf(event, "Verdict scope", reason, "markreason")
        if r["issue"]:
            leaf(event, "Import warning", r["issue"], "issue")
        meta = EventFields(r["headers"], r["raw"])
        for title, pairs in [
            (
                "Identity and network",
                [
                    ("IP address", "ip"),
                    ("Device ID", "device"),
                    ("Session ID", "session"),
                    ("Location", "location"),
                    ("Operating system", "os"),
                    ("Request ID", "request"),
                    ("Correlation ID", "correlation"),
                ],
            ),
            (
                "Authentication and application",
                [
                    ("Application", "application"),
                    ("Application ID", "appid"),
                    ("Client", "client"),
                    ("Authentication method", "authmethod"),
                    ("Conditional Access", "conditionalaccess"),
                    ("Service principal", "serviceprincipal"),
                ],
            ),
            (
                "Mailbox and directory",
                [
                    ("Workload", "workload"),
                    ("Recipient", "recipient"),
                    ("Subject", "subject"),
                    ("Message ID", "messageid"),
                    ("Rule", "rule"),
                    ("Mail access", "access"),
                    ("Folder", "folder"),
                    ("Directory target", "target"),
                ],
            ),
        ]:
            entries = []
            for label, field in pairs:
                vals = (
                    r["ip"].split(", ")
                    if field == "ip"
                    else [r["request"]] if field == "request" else meta.values(field)
                )
                entries.extend((label, v, field) for v in vals if v)
            if entries:
                section = group(title, entries[0][1], entries[0][2])
                for label, v, field in entries:
                    leaf(section, label, v, field)
        provenance = group("Evidence location", f"{r['name']} · row {r['rownum']}")
        provenance.setData(0, FILTER_ROLE, evidence_row_predicate(r))
        for label, v, field in [
            ("File", r["name"], "file"),
            ("Sheet", r["sheet"], "sheet"),
            ("Row", r["rownum"], "row"),
            ("Original path", r["path"], "path"),
            ("SHA-256", r["digest"], "sha256"),
        ]:
            leaf(provenance, label, v, field)
        if r["request"] and r["kind"].startswith(("Interactive ", "Non-interactive ")):
            target = r["kind"].split()[0] + (
                " sign-ins" if r["kind"].endswith("details") else " auth details"
            )
            rows = db.execute(
                """SELECT e.id,e.rownum,l.name,l.sheet,f.path,f.digest,e.op,e.result
                FROM events e JOIN logs l ON l.id=e.log JOIN files f ON f.id=l.file
                WHERE e.request!='' AND e.request=? AND l.kind=? ORDER BY e.time,e.id""",
                (r["request"], target),
            ).fetchall()
            section = group("Related authentication records", f"{len(rows)} matching")
            section.setData(0, FILTER_ROLE, f"request = {quote(r['request'])}")
            for link in rows:
                n = leaf(section, f"Row {link['rownum']}", link["op"] + " · " + link["result"])
                n.setData(1, ROLE, link["id"])
                n.setData(0, FILTER_ROLE, evidence_row_predicate(link))
                n.setToolTip(0, link["name"] + " · Double-click to open")
        self.render()

    def follow(self, node, *_):
        key = node.data(1, ROLE)
        if key:
            self.followEvent.emit(key)

    def select_detail(self, node, *_):
        p = node.data(0, ROLE)
        if not p:
            return
        # Select the original cell.
        field, wanted = p
        if not field:
            return
        expected = set(SLOTS.get(field, ((), ()))[0])
        expected.update(
            {
                "ip": {"IP address", "IPAddress"},
                "actor": {"Username", "UserId", "ActorDisplayName", "ActorUserPrincipalName"},
                "time": {"Date (UTC)", "Date", "CreationDate"},
                "operation": {"Operation", "Activity"},
                "result": {"Status", "Result", "Succeeded"},
                "request": {"Request ID"},
            }.get(field, ())
        )
        for i, (name, val) in enumerate(self.sources):
            if name in expected and str(val).casefold() == wanted.casefold():
                self.source.setCurrentIndex(i + 1)
                break
        else:
            idx = self.source.findText("AuditData")
            if idx < 0 or self.mode.currentIndex() != 0:
                return
            names = set(SLOTS.get(field, ((), ()))[1])
            names.update(
                {
                    "session": {"AADSessionId"},
                    "ip": {"ClientIP", "ClientIPAddress", "ActorIpAddress"},
                    "actor": {"UserId"},
                    "operation": {"Operation"},
                    "result": {"ResultStatus"},
                    "recipient": {"Address"},
                    "rule": {"Value"},
                    "folder": {"Path"},
                    "subject": {"Subject"},
                }.get(field, ())
            )
            if not names:
                return
            self.source.setCurrentIndex(idx)

            def locate(node):
                _, val, _, _ = self.nodes[id(node)]
                if node.text(0) in names and str(val).casefold() == wanted.casefold():
                    return node
                self.fill_branch(node)
                for j in range(node.childCount()):
                    found = locate(node.child(j))
                    if found:
                        return found

            if self.fields.topLevelItemCount():
                found = locate(self.fields.topLevelItem(0))
                if found:
                    self.fields.setCurrentItem(found)
                    self.fields.scrollToItem(found)

    def current_source(self):
        i = self.source.currentData()
        if i is None or i < 0:
            return None
        return self.sources[i]

    def current_text(self):
        selected = self.current_source()
        if selected:
            return serialize(selected[1])
        out = io.StringIO(newline="")
        writer = csv.writer(out)
        writer.writerow(self.headers)
        writer.writerow(self.values)
        return out.getvalue()

    def copy_source(self):
        copy(self.text.toPlainText() if self.mode.currentIndex() != 0 else self.current_text())

    def render(self, *_):
        self.fields.clear()
        self.nodes.clear()
        self.find_status.clear()
        if not self.sources:
            self.text.clear()
            return
        selected = self.current_source()
        mode = self.mode.currentIndex()
        self.pages.setCurrentIndex(0 if mode == 0 else 1)
        if mode:
            if mode == 1 and selected:
                value = json.dumps(decode(selected[1]), ensure_ascii=False, indent=2)
            elif mode == 1:
                value = json.dumps(
                    [{"field": n, "value": decode(v)} for n, v in self.sources],
                    ensure_ascii=False,
                    indent=2,
                )
            else:
                value = self.current_text()
            if mode == 1:
                value = value.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
            self.text.setPlainText(value)
            return
        indices = range(len(self.sources)) if selected is None else [self.source.currentData()]
        groups = {}
        lookup = dict(self.sources)
        self.rendering = True
        for i in indices:
            name, val = self.sources[i]
            header = self.headers[i] if i < len(self.headers) else None
            pointer = self.pointers[i]
            if self.hidden_empty and (val is None or val == ""):
                continue
            parent = self.fields
            prefix = ()
            if selected is None and len(self.sources) > 6:
                for section in sections(header or name, lookup):
                    prefix += (section,)
                    if prefix not in groups:
                        branch = QTreeWidgetItem(parent, [section, ""])
                        path = " / ".join(prefix)
                        self.nodes[id(branch)] = (branch, {}, path, None)
                        groups[prefix] = branch
                    parent = groups[prefix]
            node = self.append_node(parent, name, decode(val), name, pointer)
            if selected and node:
                node.setExpanded(True)
        for prefix, branch in groups.items():
            branch.setText(1, str(branch.childCount()) + " entries")
            if prefix == ("Event fields",) or (" / ".join(prefix)) in self.expanded:
                branch.setExpanded(True)
        for i in range(self.fields.topLevelItemCount()):
            self.restore_branch(self.fields.topLevelItem(i))
        self.rendering = False

    def restore_branch(self, node):
        if self.nodes[id(node)][2] in self.expanded:
            node.setExpanded(True)
        if node.isExpanded():
            for j in range(node.childCount()):
                self.restore_branch(node.child(j))

    def open_branch(self, node):
        self.fill_branch(node)
        if not self.rendering and id(node) in self.nodes:
            self.expanded.add(self.nodes[id(node)][2])

    def close_branch(self, node):
        if not self.rendering and id(node) in self.nodes:
            self.expanded.discard(self.nodes[id(node)][2])

    def append_node(self, parent, name, val, path, pointer):
        if self.hidden_empty and (val is None or val == ""):
            return None
        summary = show_value(val)
        node = QTreeWidgetItem(parent, [str(name), summary.replace("\n", " ↵ ")[:300]])
        node.setToolTip(0, path)
        node.setToolTip(1, summary[:4096])
        self.nodes[id(node)] = (node, val, path, pointer)
        if isinstance(val, (dict, list)) and val:
            node.setChildIndicatorPolicy(QTreeWidgetItem.ChildIndicatorPolicy.ShowIndicator)
        return node

    def fill_branch(self, node):
        entry = self.nodes.get(id(node))
        if not entry or node.childCount():
            return
        _, value, path, pointer = entry
        entries = (
            value.items()
            if isinstance(value, dict)
            else enumerate(value) if isinstance(value, list) else ()
        )
        for k, v in entries:
            next_path = f"{path}[{k}]" if isinstance(value, list) else f"{path}.{k}"
            part = "*" if isinstance(value, list) else escape_pointer(k)
            self.append_node(
                node, k, decode(v), next_path, pointer + "/" + part if pointer is not None else None
            )

    def expand_loaded(self):
        for i in range(self.fields.topLevelItemCount()):
            node = self.fields.topLevelItem(i)
            node.setExpanded(True)
            for j in range(node.childCount()):
                node.child(j).setExpanded(True)

    def find_next(self):
        term = self.find.text().casefold()
        if not term:
            return
        if self.pages.currentIndex() == 1:
            if not self.text.find(self.find.text()):
                from PySide6.QtGui import QTextCursor

                self.text.moveCursor(QTextCursor.MoveOperation.Start)
                self.find_status.setText("" if self.text.find(self.find.text()) else "No match")
            return
        matches = []

        def visit(node):
            _, val, path, _ = self.nodes[id(node)]
            if term in node.text(0).casefold() or (
                not isinstance(val, (list, dict)) and term in str(val).casefold()
            ):
                matches.append(node)
            self.fill_branch(node)
            for i in range(node.childCount()):
                visit(node.child(i))

        for i in range(self.fields.topLevelItemCount()):
            visit(self.fields.topLevelItem(i))
        if not matches:
            self.find_status.setText("No match")
            return
        current = self.fields.currentItem()
        index = (matches.index(current) + 1) % len(matches) if current in matches else 0
        hit = matches[index]
        parent = hit.parent()
        while parent:
            parent.setExpanded(True)
            parent = parent.parent()
        self.fields.setCurrentItem(hit)
        self.fields.scrollToItem(hit)
        self.find_status.setText(f"{index+1}/{len(matches)}")

    def formatted_targets(self):
        selected = self.source.currentData()
        if selected >= 0:
            yield from json_line_targets(decode(self.sources[selected][1]), self.pointers[selected])
            return
        # Map wrapper lines to columns.
        yield None, None
        for (_, raw), pointer in zip(self.sources, self.pointers):
            value = decode(raw)
            yield pointer, value
            yield pointer, value
            yield from json_line_targets(value, pointer)
            yield pointer, value
        yield None, None

    def text_predicate(self, cursor):
        if not self.sources:
            return None
        if self.mode.currentIndex() == 1:
            target = next(islice(self.formatted_targets(), cursor.blockNumber(), None), None)
            return source_predicate(*target) if target and target[0] is not None else None
        index = self.source.currentData()
        if index < 0:
            start = QTextCursor(cursor)
            start.movePosition(QTextCursor.MoveOperation.Start, QTextCursor.MoveMode.KeepAnchor)
            row, index = csv_cell_at(start.selectedText().replace("\u2029", "\n"))
            if row > 1 or index >= len(self.pointers):
                return None
            if row == 0:
                return f"exists {quote(self.pointers[index])}" if self.pointers[index] is not None else None
        pointer = self.pointers[index]
        if pointer is None:
            return None
        raw = self.sources[index][1]
        selection = self.text.textCursor()
        if selection.selectionStart() <= cursor.position() < selection.selectionEnd():
            fragment = selection.selectedText().replace("\u2029", "\n")
            if fragment and fragment in raw:
                return f"field {quote(pointer)} contains {quote(fragment)}"
        return source_predicate(pointer, decode(raw))

    def text_context(self, pos):
        menu = QMenu(self.text)
        standard = self.text.createStandardContextMenu(pos)
        expr = self.text_predicate(self.text.cursorForPosition(pos))
        if expr:
            self.filterMenuRequested.emit(menu, expr)
            menu.addSeparator()
        menu.addActions(standard.actions())
        menu.exec(self.text.viewport().mapToGlobal(pos))
        menu.deleteLater()
        standard.deleteLater()

    def context(self, view, pos):
        node = view.itemAt(pos)
        if not node:
            return
        view.setCurrentItem(node)
        menu = QMenu(view)
        data = node.data(0, ROLE) if view is self.details else None
        if view is self.fields:
            _, val, path, pointer = self.nodes[id(node)]
            if pointer is not None:
                self.filterMenuRequested.emit(menu, source_predicate(pointer, val))
                if not isinstance(val, (list, dict)):
                    menu.addAction(
                        "Apply as column", lambda: self.columnRequested.emit(node.text(0), pointer)
                    )
                menu.addSeparator()
                menu.addAction("Copy BQL field path", lambda: copy(pointer))
            menu.addAction("Copy source location", lambda: copy(path))
            menu.addAction("Copy value", lambda: copy(serialize(val)))
        else:
            expr = node.data(0, FILTER_ROLE)
            if data and data[0]:
                expr = f"{data[0]} = {quote(data[1])}"
            if expr:
                self.filterMenuRequested.emit(menu, expr)
            if data:
                menu.addAction("Copy value", lambda: copy(data[1]))
        if node.data(1, ROLE):
            menu.addAction("Go to related event", lambda: self.follow(node))
        if (
            node.childCount()
            or node.childIndicatorPolicy() == QTreeWidgetItem.ChildIndicatorPolicy.ShowIndicator
        ):
            menu.addAction(
                "Collapse branch" if node.isExpanded() else "Expand branch",
                lambda: node.setExpanded(not node.isExpanded()),
            )
        if not menu.isEmpty():
            menu.exec(view.viewport().mapToGlobal(pos))
        menu.deleteLater()
