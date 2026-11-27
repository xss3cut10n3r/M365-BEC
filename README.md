# M365 Email Log Analyzer

An offline Windows tool for investigating Microsoft 365 business email compromise. Review Purview audit logs, Entra sign-in and directory audit logs, and Exchange message traces on one UTC timeline.

[Download M365-BEC.exe for Windows x64](https://github.com/xss3cut10n3r/M365-BEC/releases/latest/download/M365-BEC.exe)

## Investigate a case

1. Run `M365-BEC.exe` and click **Open logs**. Select CSV or Excel (`.xlsx`) files from one tenant.
2. Save the `.bec` case when prompted. It stores the original files and your verdicts. Click **Open case** to resume later.
3. In the **View** menu, choose **All case events**. Select an event to see its original row in **Source record**.
4. To filter by IP, right-click an IP address, open **Apply as Filter**, and choose **Selected**. Use the **Edit** menu to mark an IP or a time range.
5. In the **File** menu, choose **Export displayed events** to save the filtered rows as CSV or Excel.

## Display filters

To find inbox rule changes for one mailbox, enter this BQL query above the timeline and press **Enter**.

```text
mailbox = "analyst@example.org" AND operation contains "InboxRule"
```

For more fields, open **BQL display filter reference** in the **Help** menu. Use **Statistics** to review IPs, sign-ins, and mailbox activity.

## Build from source

Install Python 3.13 on Windows x64, then run these commands from the repository folder.

```powershell
python -m pip install -r requirements-build.txt
.\build.cmd
```

The executable is written to `dist\desktop\M365-BEC.exe`.
