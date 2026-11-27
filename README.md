# M365 Email Log Analyzer

<p align="center"><img src="assets/screenshot.png" alt="Microsoft 365 events on one timeline, with marked rows and the original record below" width="100%"></p>

Wireshark for Microsoft 365 email compromise. Open your Purview, Entra, and Exchange exports and read them as one UTC timeline. Filter by anything, mark what is malicious, and export the findings.

Offline, for Windows and Linux. Logs are copied into one case file and your originals are never touched.

**[Windows x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-windows-x64.zip)** | **[Linux x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC-linux-x64.tar.gz)**

## Use

Extract, run `M365-BEC.exe` (Linux: `./m365-bec`), and click **Open logs**. CSV, Excel, and JSON all work.

Right-click any value to filter on it, or type a filter above the timeline:

```text
mailbox = "analyst@example.org" AND operation contains "InboxRule"
```

## Build

C17, GTK 4, SQLite, CMake. Run `bash packaging/windows.sh` in an [MSYS2](https://www.msys2.org) UCRT64 shell, or `bash packaging/linux.sh` on Ubuntu 24.04 or newer. Needs zlib, Expat, libzip, libxlsxwriter, and yyjson.

[GPL-3.0](LICENSE)
