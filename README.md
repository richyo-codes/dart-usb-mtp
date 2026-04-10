# usb_sync

Reusable Flutter plugin for USB device discovery and file access.

Status:
- API scaffolded
- Linux native implementation present
- Android MTP implementation present (`openDevice`, `listEntries`, `readBytes`, `closeSession`)
- Windows MTP implementation present (`listDevices`, `openDevice`, `listEntries`, `readBytes`, `closeSession`)
- Android/Linux mass-storage implementations pending
- `watchDevices` emits attach/detach/changed events using polling-diff today
- Native OS event streams can be layered in later without changing app API
- Linux MTP: detects existing GVFS mounts and can trigger `gio mount` via `openDevice`
- `example/` app included for manual device discovery and file-browsing tests

Designed usage:
- app code provides filters (for example Garmin VID/PID)
- package stays vendor-agnostic

Example app:
- `cd example`
- `flutter run -d windows`, `flutter run -d linux`, or `flutter run -d android`
- use the filter panel to narrow by VID/PID or name pattern
- open a device session and browse storage roots/directories
