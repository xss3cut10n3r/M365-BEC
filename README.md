# M365 Email Log Analyzer

Offline Windows desktop tool for reviewing Microsoft 365 business email compromise evidence.

## Open a case

Launch **dist/desktop/M365-BEC.exe**. Choose **File → Open logs** and select CSVs or Excel workbooks from one tenant. Create a local **.bec** case when prompted. Reopen it with **File → Open case**. You can also drop a case or a set of log files onto the window.

Each CSV and populated workbook sheet gets a tab. Header-only reports open with zero rows. **View → All case events** searches across files. The virtual event list scrolls continuously. Click a column header to sort the full result, drag headers to reorder them, or right-click a header to change visible columns. The status bar shows total records, displayed records, and export-limit warnings.

## Review evidence

Timeline columns share the window width. Account, filename, and operation columns get more space as the window grows. Narrow windows scroll horizontally once the columns reach readable widths. Drag a column divider to choose a width. Right-click a timeline header and choose **Fit columns to window** to restore automatic sizing.

Each log tab has a close button. Use **+** beside the tabs or **View → Open log tab** to open a log already in the case. **Ctrl+Shift+T** restores the last closed tab with its filter and selection. Close **Event details** or **Source record** using the × in its header. Reopen either from **View**. The app remembers your pane layout between sessions.

**Source** identifies the log behind each event, such as **Entra · Interactive sign-ins** or **Purview audit**. **Source file** names the imported CSV or workbook. Select a row for its sheet and row number, or right-click either column to filter the timeline.

**All case events** opens oldest UTC time first. **ID** numbers events across the whole case from 1. Equal timestamps follow import order. Undated events come last. IDs follow each event across tabs, filters, and sorts. Importing earlier evidence renumbers the timeline. Verdicts stay attached to the same underlying events. Exports include both the timeline ID and the original source row.

Type a BQL expression above the grid and press Enter.

~~~text
mailbox = "analyst@example.org" AND operation = "Send"
time >= "2026-01-01" AND time < "2026-01-02"
ip = "203.0.113.7" OR marked = true
verdict = "Unsure" AND id >= 100
operation contains "InboxRule" AND NOT result = "Failure"
device = "device-id" OR session = "session-id"
field "/AuditData/Parameters/*/Name" = "MoveToFolder"
exists "/AuditData/Parameters"
~~~

**Help → BQL display filter reference** lists the searchable fields. **Analyze → Save display filter in case** stores a named query. The bookmark beside the display filter opens saved queries, investigation examples, and recent filters.

Select an event to inspect its identity, network, mailbox, source file, worksheet, and original row. Double-click a related authentication record to open its source tab. Right-click events to mark the selection, all activity from selected IPs, or a UTC time range. Choose **Good** (neon green), **Bad** (neon red), **Unsure** (yellow), or **Clear verdict** (neutral). Unmarked events have no verdict color. The latest matching action wins. IP and time rules also cover later imports. Time ranges have an exclusive end. **Ctrl+Z** undoes a whole verdict action. Older malicious marks appear as **Bad**.

Right-click a value in **Event details** or **Source record** and choose **Apply as Filter → Selected**. **Not selected** excludes it. With a filter active, choose **… and selected** to narrow the results. **Prepare a Filter** puts the expression in the BQL bar for editing. **Find matching events across case** searches all log tabs. Normalized details such as IP use shared fields. Source values use their original field paths. Right-clicking a JSON object or array tests for its presence. These actions also work on formatted JSON lines and CSV cells. Select part of a raw cell to search that field for the selected text.

In **Source record**, choose the whole imported row or one column. **Field tree** expands objects, arrays, and numbered directory targets. **Formatted JSON** preserves duplicate source columns as separate entries. **Row / cell text** shows the selected cell or reconstructs CSV from the decoded header and row. Use **Save original evidence copy** for byte-exact evidence. **Ctrl+F** searches the selected data source. Right-click a scalar field to copy it, filter by it, or add it as a sortable timeline column. Source paths support array wildcards and duplicate header ordinals such as <code>/@2:Incoming token type</code>.

**Analyze → Evidence and import details** shows file counts, row issues, cap warnings, and authentication-step matches. **File → Export displayed events** writes the full filtered result in its current sort order, including custom columns, to CSV or XLSX.

## Analyze telemetry

The **Statistics** menu includes activity graphs, IP endpoints, account–IP pairs, senders and recipients, sign-in outcomes, authentication methods, devices, sessions, Conditional Access, application grants, inbox rules, accessed folders and messages, and analyst marks. **Investigation indicators** groups observable operations for triage.

Choose a log or the whole case and toggle **Limit to display filter**. Each report counts source events across the selected scope. A message with multiple recipients contributes once to each distinct sender–recipient pair. Groups can overlap. Percentages use the displayed-event count as their denominator. Reports show the first and last observed times in UTC. Double-click a group or chart interval to open its supporting events, or save the sorted statistics as CSV.

## Data handling

The SQLite case contains original files, decoded records, source paths, and analyst marks. Treat it as sensitive evidence. Close the app before copying a case. The importer stages each input locally, commits one file atomically, and skips unchanged files from the same path. Malformed CSV syntax fails that file's transaction. Rows with inconsistent widths or malformed audit payloads remain visible with an import issue.

The importer supports UTF-8 and BOM-tagged UTF-16 CSV, and XLSX. It recognizes the supplied Entra and Purview layouts. Exchange enhanced summary support was tested with synthetic exports. The message trace report separates each recorded delivery outcome when recipients have different statuses. Validation against a real Exchange trace is pending. The timeline expects ISO dates for normalized time sorting. Original timestamp text stays available for inspection. Use new cases for different tenants. [Microsoft Exchange trace fields](https://learn.microsoft.com/en-us/exchange/monitoring/trace-an-email-message/message-trace-modern-eac)

## Run from source

On Windows x64 with Python 3.13 installed:

~~~bat
python -m pip install -r requirements-build.txt
python -m unittest -v test_case_store test_desktop
python analyzer.py
build.cmd
~~~

The single executable bundles Python, Qt Widgets, SQLite, and the workbook reader. It runs without a separate Python or Office installation. The pane layout, field inspection, display filters, and statistics workflows were informed by Wireshark's Qt frontend. [Wireshark UI source](https://github.com/wireshark/wireshark/tree/master/ui/qt)
