# M365 Email Log Analyzer

An offline tool for investigating Microsoft 365 business email compromise on Windows and Linux. Review Purview audit logs, Entra sign-in and directory audit logs, and Exchange message traces on one UTC timeline.

- [Download for Windows x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-windows-x64.zip). Extract the zip and run `M365-BEC.exe`.
- [Download for Linux x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-linux-x64.tar.gz). Extract it and run `./m365-bec`. Needs Ubuntu 24.04 or a distribution of the same age or newer.

## Investigate a case

1. Run `M365-BEC.exe` (Linux: `./m365-bec`) and click **Open logs**. Select CSV, Excel (`.xlsx`), or JSON files from one tenant.
2. Save the `.bec` case when prompted. The case is one standalone file. It keeps sealed copies of the logs and your verdicts, and saves as you work. Original log files are only read. The open case shows in the title bar and status bar. Click **Open case** to resume later.
3. In the **View** menu, choose **All case events**. Select an event to see its original record in **Source record**.
4. To filter by IP, right-click an IP address, open **Apply as Filter**, and choose **Selected**. Use the **Edit** menu to mark an IP or a time range.
5. In the **File** menu, choose **Export displayed events** to save the filtered rows as CSV, Excel, or JSON.

## Display filters

To find inbox rule changes for one mailbox, enter this BQL query above the timeline and press **Enter**.

```text
mailbox = "analyst@example.org" AND operation contains "InboxRule"
```

Nested fields use a path. This matches Graph sign-ins with one error code.

```text
field "/status/errorCode" = "50126"
```

For more fields, open **BQL display filter reference** in the **Help** menu. Use **Statistics** to review IPs, sign-ins, and mailbox activity.

## JSON logs

Import one event object, an array of events, or a Microsoft Graph export with events in `value`. Each event keeps its file name and JSON path, such as `/value/17`. Nested objects, arrays, nulls, and number text are preserved. Malformed or ambiguous files are rejected and the case is left unchanged.

JSON exports are a UTF-8 array in the displayed order. Each entry holds the UTC time, source, original location, verdict, and the original record. The analyzer reads its own JSON exports and restores their verdicts.

## Command line

```text
M365-BEC --case case.bec logs.csv signins.json          open the window and import
M365-BEC --case case.bec --filter "verdict = Bad"       open with a filter
M365-BEC --no-gui --case case.bec logs.xlsx             import, then exit
M365-BEC --case case.bec --filter "ip = 203.0.113.7" --export hits.json
```

`--export` writes CSV, XLSX, or JSON by file extension, then exits.

## Build from source

The app is C17 with GTK 4, SQLite, and CMake.

Windows x64: install [MSYS2](https://www.msys2.org), then run this in the UCRT64 shell.

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,gtk4,sqlite,libxlsxwriter,libzip,yyjson,ntldd}
bash packaging/windows.sh
```

The package is written to `dist/windows/M365-BEC`. Run `M365-BEC.exe` from that folder.

Linux x64 (Ubuntu 24.04 or newer):

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev libsqlite3-dev zlib1g-dev libexpat1-dev libzip-dev libxlsxwriter-dev
bash packaging/linux.sh
```

The package is written to `dist/linux/M365-BEC`. Run `./m365-bec` from that folder.

## Tests

```sh
ctest --test-dir build/win --output-on-failure      # native regression tests
python tests/parity.py build/win/bec-tool.exe       # compare against the Python reference
```

The Python reference app (`analyzer.py`) stays in this repository. Build it with `python -m pip install -r requirements-build.txt` and `.\build.cmd`.
