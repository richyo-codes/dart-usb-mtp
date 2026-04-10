import "dart:async";
import "dart:convert";
import "dart:typed_data";

import "package:flutter/material.dart";
import "package:usb_sync/usb_sync.dart";

void main() {
  runApp(const UsbSyncExampleApp());
}

class UsbSyncExampleApp extends StatelessWidget {
  const UsbSyncExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    final theme = ThemeData(
      colorScheme: ColorScheme.fromSeed(
        seedColor: const Color(0xFF0D9488),
        brightness: Brightness.light,
      ),
      scaffoldBackgroundColor: const Color(0xFFF3F7F6),
      useMaterial3: true,
    );

    return MaterialApp(
      title: "USB Sync Example",
      debugShowCheckedModeBanner: false,
      theme: theme,
      home: const UsbSyncHomePage(),
    );
  }
}

class UsbSyncHomePage extends StatefulWidget {
  const UsbSyncHomePage({super.key});

  @override
  State<UsbSyncHomePage> createState() => _UsbSyncHomePageState();
}

class _UsbSyncHomePageState extends State<UsbSyncHomePage> {
  static const int _previewLimitBytes = 256 * 1024;

  final TextEditingController _vendorIdController = TextEditingController();
  final TextEditingController _productIdController = TextEditingController();
  final TextEditingController _productPatternController =
      TextEditingController();
  final TextEditingController _manufacturerPatternController =
      TextEditingController();

  StreamSubscription<UsbDeviceEvent>? _watchSubscription;

  UsbFilter _activeFilter = const UsbFilter(
    transports: <UsbTransport>[UsbTransport.mtp],
  );
  String _transportFilter = "mtp";
  bool _loadingDevices = false;
  bool _openingSession = false;
  bool _loadingEntries = false;
  bool _readingFile = false;

  List<UsbDeviceInfo> _devices = const <UsbDeviceInfo>[];
  List<UsbEntry> _entries = const <UsbEntry>[];
  final List<String> _logLines = <String>[];

  UsbSession? _session;
  String _currentPath = "/";
  String? _selectedDeviceId;
  String? _previewPath;
  String? _previewText;
  int? _previewBytesLength;

  @override
  void initState() {
    super.initState();
    _restartWatcher();
    unawaited(_refreshDevices());
  }

  @override
  void dispose() {
    _watchSubscription?.cancel();
    final sessionId = _session?.sessionId;
    if (sessionId != null) {
      unawaited(UsbSync.closeSession(sessionId: sessionId));
    }
    _vendorIdController.dispose();
    _productIdController.dispose();
    _productPatternController.dispose();
    _manufacturerPatternController.dispose();
    super.dispose();
  }

  Future<void> _refreshDevices() async {
    setState(() {
      _loadingDevices = true;
    });

    try {
      final devices = await UsbSync.listDevices(filter: _activeFilter);
      if (!mounted) {
        return;
      }
      setState(() {
        _devices = devices;
        if (_selectedDeviceId != null &&
            !_devices.any((device) => device.id == _selectedDeviceId)) {
          _selectedDeviceId = null;
        }
      });
    } catch (error) {
      _appendLog("listDevices failed: $error");
    } finally {
      if (mounted) {
        setState(() {
          _loadingDevices = false;
        });
      }
    }
  }

  void _restartWatcher() {
    _watchSubscription?.cancel();
    _watchSubscription = UsbSync.watchDevices(filter: _activeFilter).listen(
      (UsbDeviceEvent event) {
        _appendLog(
          "watchDevices ${event.type.name}: ${event.device.productName ?? event.device.id}",
        );
        unawaited(_refreshDevices());
      },
      onError: (Object error) {
        _appendLog("watchDevices error: $error");
      },
    );
  }

  Future<void> _applyFilters() async {
    try {
      final UsbFilter filter = _buildFilter();
      setState(() {
        _activeFilter = filter;
      });
      _appendLog("Applied filter: ${_describeFilter(filter)}");
      _restartWatcher();
      await _refreshDevices();
    } on FormatException catch (error) {
      _appendLog(error.message);
    }
  }

  UsbFilter _buildFilter() {
    final int? vendorId = _parseHexOrNull(
      _vendorIdController.text,
      fieldName: "Vendor ID",
    );
    final int? productId = _parseHexOrNull(
      _productIdController.text,
      fieldName: "Product ID",
    );

    final List<UsbTransport> transports = switch (_transportFilter) {
      "mtp" => const <UsbTransport>[UsbTransport.mtp],
      "massStorage" => const <UsbTransport>[UsbTransport.massStorage],
      _ => const <UsbTransport>[],
    };

    return UsbFilter(
      vendorIds: vendorId == null ? const <int>[] : <int>[vendorId],
      productIds: productId == null ? const <int>[] : <int>[productId],
      transports: transports,
      productNamePattern: _trimToNull(_productPatternController.text),
      manufacturerPattern: _trimToNull(_manufacturerPatternController.text),
    );
  }

  int? _parseHexOrNull(String raw, {required String fieldName}) {
    final String trimmed = raw.trim();
    if (trimmed.isEmpty) {
      return null;
    }
    final String normalized =
        trimmed.startsWith("0x") || trimmed.startsWith("0X")
        ? trimmed.substring(2)
        : trimmed;
    try {
      return int.parse(normalized, radix: 16);
    } on FormatException {
      throw FormatException(
        "$fieldName must be hexadecimal, for example 091e.",
      );
    }
  }

  String? _trimToNull(String value) {
    final String trimmed = value.trim();
    return trimmed.isEmpty ? null : trimmed;
  }

  String _describeFilter(UsbFilter filter) {
    final List<String> parts = <String>[];
    if (filter.vendorIds.isNotEmpty) {
      parts.add(
        "VID ${filter.vendorIds.first.toRadixString(16).padLeft(4, "0")}",
      );
    }
    if (filter.productIds.isNotEmpty) {
      parts.add(
        "PID ${filter.productIds.first.toRadixString(16).padLeft(4, "0")}",
      );
    }
    if (filter.transports.isNotEmpty) {
      parts.add("transport ${filter.transports.first.name}");
    }
    if (filter.productNamePattern != null) {
      parts.add("product /${filter.productNamePattern}/");
    }
    if (filter.manufacturerPattern != null) {
      parts.add("maker /${filter.manufacturerPattern}/");
    }
    return parts.isEmpty ? "no filters" : parts.join(", ");
  }

  Future<void> _openDevice(UsbDeviceInfo device) async {
    setState(() {
      _openingSession = true;
      _selectedDeviceId = device.id;
    });

    try {
      if (_session != null) {
        await UsbSync.closeSession(sessionId: _session!.sessionId);
      }

      final UsbSession session = await UsbSync.openDevice(deviceId: device.id);
      if (!mounted) {
        return;
      }
      setState(() {
        _session = session;
        _currentPath = "/";
        _entries = const <UsbEntry>[];
        _previewPath = null;
        _previewText = null;
        _previewBytesLength = null;
      });
      _appendLog(
        "Opened session ${session.sessionId} for ${device.productName ?? device.id}",
      );
      await _loadEntries("/");
    } catch (error) {
      _appendLog("openDevice failed: $error");
    } finally {
      if (mounted) {
        setState(() {
          _openingSession = false;
        });
      }
    }
  }

  Future<void> _closeSession() async {
    final UsbSession? session = _session;
    if (session == null) {
      return;
    }
    try {
      await UsbSync.closeSession(sessionId: session.sessionId);
      _appendLog("Closed session ${session.sessionId}");
    } catch (error) {
      _appendLog("closeSession failed: $error");
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _session = null;
      _entries = const <UsbEntry>[];
      _currentPath = "/";
      _previewPath = null;
      _previewText = null;
      _previewBytesLength = null;
    });
  }

  Future<void> _loadEntries(String path) async {
    final UsbSession? session = _session;
    if (session == null) {
      return;
    }

    setState(() {
      _loadingEntries = true;
    });

    try {
      final List<UsbEntry> entries = await UsbSync.listEntries(
        sessionId: session.sessionId,
        path: path,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _entries = entries;
        _currentPath = path;
      });
      _appendLog("Listed ${entries.length} entries at $path");
    } catch (error) {
      _appendLog("listEntries failed: $error");
    } finally {
      if (mounted) {
        setState(() {
          _loadingEntries = false;
        });
      }
    }
  }

  Future<void> _readPreview(UsbEntry entry) async {
    final UsbSession? session = _session;
    if (session == null || entry.isDirectory) {
      return;
    }

    if (entry.sizeBytes != null && entry.sizeBytes! > _previewLimitBytes) {
      setState(() {
        _previewPath = entry.path;
        _previewText =
            "Preview disabled for files larger than ${_previewLimitBytes ~/ 1024} KB.";
        _previewBytesLength = entry.sizeBytes;
      });
      return;
    }

    setState(() {
      _readingFile = true;
      _previewPath = entry.path;
      _previewText = null;
      _previewBytesLength = null;
    });

    try {
      final Uint8List bytes = await UsbSync.readBytes(
        sessionId: session.sessionId,
        path: entry.path,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _previewText = _formatPreview(bytes);
        _previewBytesLength = bytes.length;
      });
      _appendLog("Read ${bytes.length} bytes from ${entry.path}");
    } catch (error) {
      _appendLog("readBytes failed: $error");
    } finally {
      if (mounted) {
        setState(() {
          _readingFile = false;
        });
      }
    }
  }

  String _formatPreview(Uint8List bytes) {
    if (bytes.isEmpty) {
      return "(empty file)";
    }
    if (_looksLikeText(bytes)) {
      return utf8.decode(bytes, allowMalformed: true);
    }

    final StringBuffer buffer = StringBuffer();
    final int limit = bytes.length < 512 ? bytes.length : 512;
    for (int i = 0; i < limit; i += 16) {
      final int rowEnd = i + 16 < limit ? i + 16 : limit;
      final List<String> hex = <String>[];
      for (int j = i; j < rowEnd; j++) {
        hex.add(bytes[j].toRadixString(16).padLeft(2, "0"));
      }
      buffer.writeln(
        "${i.toRadixString(16).padLeft(4, "0")}: ${hex.join(" ")}",
      );
    }
    if (limit < bytes.length) {
      buffer.writeln("...");
    }
    return buffer.toString().trimRight();
  }

  bool _looksLikeText(Uint8List bytes) {
    int printable = 0;
    final int sampleLength = bytes.length < 1024 ? bytes.length : 1024;
    for (int i = 0; i < sampleLength; i++) {
      final int byte = bytes[i];
      if (byte == 9 || byte == 10 || byte == 13 || (byte >= 32 && byte < 127)) {
        printable++;
      }
    }
    return printable / sampleLength >= 0.85;
  }

  void _appendLog(String message) {
    final DateTime now = DateTime.now();
    final String timestamp =
        "${now.hour.toString().padLeft(2, "0")}:${now.minute.toString().padLeft(2, "0")}:${now.second.toString().padLeft(2, "0")}";
    if (!mounted) {
      return;
    }
    setState(() {
      _logLines.insert(0, "[$timestamp] $message");
      if (_logLines.length > 40) {
        _logLines.removeRange(40, _logLines.length);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final Widget devicePanel = _buildDevicePanel(context);
    final Widget browserPanel = _buildBrowserPanel(context);
    final Widget previewPanel = _buildPreviewPanel(context);

    return Scaffold(
      appBar: AppBar(
        title: const Text("USB Sync Example"),
        actions: <Widget>[
          IconButton(
            onPressed: _loadingDevices ? null : _refreshDevices,
            icon: const Icon(Icons.refresh),
            tooltip: "Refresh devices",
          ),
        ],
      ),
      body: LayoutBuilder(
        builder: (BuildContext context, BoxConstraints constraints) {
          if (constraints.maxWidth >= 1280) {
            return Row(
              children: <Widget>[
                Expanded(flex: 3, child: devicePanel),
                Expanded(flex: 4, child: browserPanel),
                Expanded(flex: 3, child: previewPanel),
              ],
            );
          }

          if (constraints.maxWidth >= 860) {
            return Row(
              children: <Widget>[
                Expanded(flex: 4, child: devicePanel),
                Expanded(
                  flex: 5,
                  child: Column(
                    children: <Widget>[
                      Expanded(flex: 3, child: browserPanel),
                      Expanded(flex: 2, child: previewPanel),
                    ],
                  ),
                ),
              ],
            );
          }

          return ListView(
            padding: const EdgeInsets.all(16),
            children: <Widget>[
              SizedBox(
                height: 640,
                child: _PanelCard(
                  title: "Devices",
                  child: _buildDeviceBody(context),
                ),
              ),
              const SizedBox(height: 16),
              SizedBox(
                height: 520,
                child: _PanelCard(
                  title: "Browser",
                  child: _buildBrowserBody(context),
                ),
              ),
              const SizedBox(height: 16),
              SizedBox(
                height: 520,
                child: _PanelCard(
                  title: "Preview & Log",
                  child: _buildPreviewBody(context),
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  Widget _buildDevicePanel(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 16, 8, 16),
      child: _PanelCard(title: "Devices", child: _buildDeviceBody(context)),
    );
  }

  Widget _buildBrowserPanel(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 16, horizontal: 8),
      child: _PanelCard(title: "Browser", child: _buildBrowserBody(context)),
    );
  }

  Widget _buildPreviewPanel(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(8, 16, 16, 16),
      child: _PanelCard(
        title: "Preview & Log",
        child: _buildPreviewBody(context),
      ),
    );
  }

  Widget _buildDeviceBody(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Wrap(
          spacing: 12,
          runSpacing: 12,
          children: <Widget>[
            SizedBox(
              width: 120,
              child: TextField(
                controller: _vendorIdController,
                decoration: const InputDecoration(
                  labelText: "Vendor ID",
                  hintText: "091e",
                ),
              ),
            ),
            SizedBox(
              width: 120,
              child: TextField(
                controller: _productIdController,
                decoration: const InputDecoration(
                  labelText: "Product ID",
                  hintText: "4ee1",
                ),
              ),
            ),
            SizedBox(
              width: 190,
              child: DropdownButtonFormField<String>(
                isExpanded: true,
                value: _transportFilter,
                decoration: const InputDecoration(labelText: "Transport"),
                items: const <DropdownMenuItem<String>>[
                  DropdownMenuItem(value: "mtp", child: Text("MTP")),
                  DropdownMenuItem(
                    value: "massStorage",
                    child: Text("Mass storage"),
                  ),
                  DropdownMenuItem(value: "any", child: Text("Any")),
                ],
                onChanged: (String? value) {
                  if (value == null) {
                    return;
                  }
                  setState(() {
                    _transportFilter = value;
                  });
                },
              ),
            ),
          ],
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _productPatternController,
          decoration: const InputDecoration(
            labelText: "Product name pattern",
            hintText: "Pixel|Garmin|Phone",
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _manufacturerPatternController,
          decoration: const InputDecoration(
            labelText: "Manufacturer pattern",
            hintText: "Google|Samsung|Garmin",
          ),
        ),
        const SizedBox(height: 12),
        Wrap(
          spacing: 12,
          runSpacing: 12,
          children: <Widget>[
            FilledButton.icon(
              onPressed: _applyFilters,
              icon: const Icon(Icons.filter_alt),
              label: const Text("Apply"),
            ),
            OutlinedButton.icon(
              onPressed: () {
                _vendorIdController.clear();
                _productIdController.clear();
                _productPatternController.clear();
                _manufacturerPatternController.clear();
                setState(() {
                  _transportFilter = "mtp";
                });
                unawaited(_applyFilters());
              },
              icon: const Icon(Icons.restart_alt),
              label: const Text("Reset"),
            ),
          ],
        ),
        const SizedBox(height: 16),
        Row(
          children: <Widget>[
            Text(
              "${_devices.length} device${_devices.length == 1 ? "" : "s"}",
              style: Theme.of(context).textTheme.titleSmall,
            ),
            const SizedBox(width: 12),
            if (_loadingDevices)
              const SizedBox.square(
                dimension: 16,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
          ],
        ),
        const SizedBox(height: 12),
        Expanded(
          child: _devices.isEmpty
              ? const Center(
                  child: Text("No devices matched the current filter."),
                )
              : ListView.separated(
                  itemCount: _devices.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 12),
                  itemBuilder: (BuildContext context, int index) {
                    final UsbDeviceInfo device = _devices[index];
                    final bool selected = device.id == _selectedDeviceId;
                    final bool activeSession = _session?.deviceId == device.id;
                    return Card(
                      color: selected
                          ? Theme.of(context).colorScheme.secondaryContainer
                          : null,
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text(
                              device.productName ?? "(unnamed device)",
                              style: Theme.of(context).textTheme.titleMedium,
                            ),
                            const SizedBox(height: 4),
                            Text(
                              device.manufacturerName ?? device.id,
                              style: Theme.of(context).textTheme.bodySmall,
                            ),
                            const SizedBox(height: 8),
                            Wrap(
                              spacing: 8,
                              runSpacing: 8,
                              children: <Widget>[
                                if (device.vendorId != null)
                                  _InfoChip(
                                    label:
                                        "VID ${device.vendorId!.toRadixString(16).padLeft(4, "0")}",
                                  ),
                                if (device.productId != null)
                                  _InfoChip(
                                    label:
                                        "PID ${device.productId!.toRadixString(16).padLeft(4, "0")}",
                                  ),
                                for (final UsbTransport transport
                                    in device.transports)
                                  _InfoChip(label: transport.name),
                                for (final UsbCapability capability
                                    in device.capabilities)
                                  _InfoChip(label: capability.name),
                              ],
                            ),
                            if (device.serialNumber != null) ...<Widget>[
                              const SizedBox(height: 8),
                              Text(
                                "Serial: ${device.serialNumber}",
                                style: Theme.of(context).textTheme.bodySmall,
                              ),
                            ],
                            const SizedBox(height: 12),
                            Wrap(
                              spacing: 8,
                              runSpacing: 8,
                              children: <Widget>[
                                FilledButton(
                                  onPressed: _openingSession
                                      ? null
                                      : () => _openDevice(device),
                                  child: Text(
                                    activeSession ? "Reopen" : "Open",
                                  ),
                                ),
                                OutlinedButton(
                                  onPressed: activeSession
                                      ? _closeSession
                                      : null,
                                  child: const Text("Close"),
                                ),
                              ],
                            ),
                          ],
                        ),
                      ),
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _buildBrowserBody(BuildContext context) {
    final UsbSession? session = _session;
    if (session == null) {
      return const Center(
        child: Text("Open a device session to browse its contents."),
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Row(
          children: <Widget>[
            Expanded(
              child: Text(
                "Session ${session.sessionId}",
                style: Theme.of(context).textTheme.titleSmall,
              ),
            ),
            if (_loadingEntries)
              const SizedBox.square(
                dimension: 16,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
          ],
        ),
        const SizedBox(height: 12),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: <Widget>[
            FilledButton.icon(
              onPressed: _loadingEntries ? null : () => _loadEntries("/"),
              icon: const Icon(Icons.home),
              label: const Text("Roots"),
            ),
            OutlinedButton.icon(
              onPressed: _loadingEntries || _currentPath == "/"
                  ? null
                  : () => _loadEntries(_parentPathOf(_currentPath)),
              icon: const Icon(Icons.arrow_upward),
              label: const Text("Up"),
            ),
            OutlinedButton.icon(
              onPressed: _loadingEntries
                  ? null
                  : () => _loadEntries(_currentPath),
              icon: const Icon(Icons.refresh),
              label: const Text("Reload"),
            ),
          ],
        ),
        const SizedBox(height: 12),
        SelectableText(
          _currentPath,
          style: Theme.of(context).textTheme.bodyMedium,
        ),
        const SizedBox(height: 12),
        Expanded(
          child: _entries.isEmpty
              ? const Center(child: Text("No entries in this location."))
              : ListView.separated(
                  itemCount: _entries.length,
                  separatorBuilder: (_, _) => const Divider(height: 1),
                  itemBuilder: (BuildContext context, int index) {
                    final UsbEntry entry = _entries[index];
                    return ListTile(
                      leading: Icon(
                        entry.isDirectory
                            ? Icons.folder_open
                            : Icons.description,
                      ),
                      title: Text(entry.name),
                      subtitle: Text(_entrySubtitle(entry)),
                      trailing: entry.isDirectory
                          ? const Icon(Icons.chevron_right)
                          : const Icon(Icons.visibility),
                      onTap: () {
                        if (entry.isDirectory) {
                          unawaited(_loadEntries(entry.path));
                        } else {
                          unawaited(_readPreview(entry));
                        }
                      },
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _buildPreviewBody(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Text(
          _previewPath == null ? "No file selected" : "Preview: $_previewPath",
          style: Theme.of(context).textTheme.titleSmall,
        ),
        const SizedBox(height: 8),
        if (_readingFile)
          const LinearProgressIndicator(minHeight: 2)
        else
          const SizedBox(height: 2),
        const SizedBox(height: 12),
        Expanded(
          child: Container(
            width: double.infinity,
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Theme.of(context).colorScheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(12),
            ),
            child: SingleChildScrollView(
              child: SelectableText(
                _previewText ??
                    "Tap a file to read a preview. Binary files fall back to a hex dump.",
                style: Theme.of(
                  context,
                ).textTheme.bodySmall?.copyWith(fontFamily: "monospace"),
              ),
            ),
          ),
        ),
        const SizedBox(height: 12),
        Text(
          _previewBytesLength == null
              ? "No bytes loaded"
              : "$_previewBytesLength bytes loaded",
          style: Theme.of(context).textTheme.bodySmall,
        ),
        const SizedBox(height: 16),
        Text("Recent log", style: Theme.of(context).textTheme.titleSmall),
        const SizedBox(height: 8),
        Expanded(
          child: ListView.separated(
            itemCount: _logLines.length,
            separatorBuilder: (_, _) => const SizedBox(height: 6),
            itemBuilder: (BuildContext context, int index) {
              return Text(
                _logLines[index],
                style: Theme.of(
                  context,
                ).textTheme.bodySmall?.copyWith(fontFamily: "monospace"),
              );
            },
          ),
        ),
      ],
    );
  }

  String _entrySubtitle(UsbEntry entry) {
    final List<String> parts = <String>[];
    parts.add(entry.path);
    if (!entry.isDirectory && entry.sizeBytes != null) {
      parts.add(_formatByteCount(entry.sizeBytes!));
    }
    if (entry.modifiedAtMs != null) {
      parts.add(
        DateTime.fromMillisecondsSinceEpoch(
          entry.modifiedAtMs!,
        ).toLocal().toString(),
      );
    }
    return parts.join(" | ");
  }

  String _formatByteCount(int bytes) {
    if (bytes < 1024) {
      return "$bytes B";
    }
    if (bytes < 1024 * 1024) {
      return "${(bytes / 1024).toStringAsFixed(1)} KB";
    }
    return "${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB";
  }

  String _parentPathOf(String path) {
    final String normalized = path == "/" ? "/" : path.replaceAll("\\", "/");
    if (normalized == "/") {
      return "/";
    }
    final int lastSlash = normalized.lastIndexOf("/");
    if (lastSlash <= 0) {
      return "/";
    }
    return normalized.substring(0, lastSlash);
  }
}

class _PanelCard extends StatelessWidget {
  const _PanelCard({required this.title, required this.child});

  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Card(
      clipBehavior: Clip.antiAlias,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(title, style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 12),
            Expanded(child: child),
          ],
        ),
      ),
    );
  }
}

class _InfoChip extends StatelessWidget {
  const _InfoChip({required this.label});

  final String label;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(label, style: Theme.of(context).textTheme.labelMedium),
    );
  }
}
