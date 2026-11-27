# M365 Email Log Analyzer

<p align="center"><img src="assets/screenshot.png" alt="Microsoft 365 events on one timeline, with marked rows and the original record below" width="100%"></p>

Wireshark for Microsoft 365 email compromise. Open your Purview audit, Entra sign-in, Entra directory audit, and Exchange message trace exports and read them as one UTC timeline. Filter by anything, mark what is malicious, and export the findings.

Logs are copied into one `.bec` case file with their SHA-256 hashes. Your original files are never changed.

To hand a case over, copy the `.bec` file while nothing is importing or marking. Close the app first to be certain.

**[Windows x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-windows-x64.zip)** | **[Linux x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-linux-x64.tar.gz)**

## Usage

1. Extract and run `M365-BEC.exe` (Linux: `./m365-bec`, or `./install-desktop.sh` for a menu launcher).
2. Click **Open logs** and pick CSV, Excel, or JSON exports. Save the case when asked.
3. Select an event to see its fields and its original record.
4. Right-click a value and choose **Apply as Filter**, or type a filter above the timeline.
5. Mark events, IPs, or time ranges good, bad, or unsure from the **Edit** menu.
6. Export the displayed events as CSV, Excel, or JSON from the **File** menu.

```text
mailbox = "analyst@example.org" AND operation contains "InboxRule"
field "/status/errorCode" = "50126"
```

The **Help** menu lists every filter field. **Statistics** breaks events down by IP, account, operation, and time.

## Command line

```sh
M365-BEC --case case.bec logs.csv signins.json
M365-BEC --case case.bec --filter "ip = 203.0.113.7" --export hits.json
M365-BEC --no-gui --case case.bec logs.xlsx
M365-BEC --case case.bec --mark bad --mark-ip 203.0.113.7
M365-BEC --case case.bec --mark good --filter "operation = \"Disable-InboxRule\""
```

`--mark` takes `good`, `bad`, `unsure`, or `clear`. Excel exports colour each row by its mark.

## Build

C17 with GTK 4, SQLite, and CMake.

Windows, in an [MSYS2](https://www.msys2.org) UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,gtk4,sqlite,libxlsxwriter,libzip,yyjson,ntldd}
bash packaging/windows.sh
```

Linux, on Ubuntu 24.04 or newer:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev libsqlite3-dev zlib1g-dev libexpat1-dev libzip-dev libxlsxwriter-dev
bash packaging/linux.sh
```

Packages are written to `dist/`.

## MCP server

**[M365-BEC-MCP](https://github.com/xss3cut10n3r/M365-BEC-MCP)** lets Claude, or any MCP client, search, mark, and export the same `.bec` cases.

[GPL-3.0](LICENSE)
