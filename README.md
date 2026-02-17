# usb_sync

Reusable Flutter plugin for USB device discovery and file access.

Status:
- API scaffolded
- Android/Linux native stubs present
- MTP and mass-storage implementations pending
- `watchDevices` emits attach/detach/changed events using polling-diff today
- Native OS event streams can be layered in later without changing app API
- Linux MTP: detects existing GVFS mounts and can trigger `gio mount` via `openDevice`

Designed usage:
- app code provides filters (for example Garmin VID/PID)
- package stays vendor-agnostic
