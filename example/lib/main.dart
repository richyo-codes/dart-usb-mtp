import "package:flutter/material.dart";
import "package:usb_mtp_client/usb_mtp_client.dart";

void main() {
  runApp(const UsbSyncExampleApp());
}

class UsbSyncExampleApp extends StatelessWidget {
  const UsbSyncExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: "USB MTP Client Example",
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0D9488),
          brightness: Brightness.light,
        ),
        useMaterial3: true,
      ),
      home: const DeviceListScreen(),
    );
  }
}

class DeviceListScreen extends StatefulWidget {
  const DeviceListScreen({super.key});

  @override
  State<DeviceListScreen> createState() => _DeviceListScreenState();
}

class _DeviceListScreenState extends State<DeviceListScreen> {
  bool _loading = false;
  String? _errorText;
  String _transportFilter = "mtp";
  List<UsbDeviceInfo> _devices = const <UsbDeviceInfo>[];

  @override
  void initState() {
    super.initState();
    _loadDevices();
  }

  Future<void> _loadDevices() async {
    setState(() {
      _loading = true;
      _errorText = null;
    });

    try {
      final List<UsbDeviceInfo> devices = await UsbSync.listDevices(
        filter: UsbFilter(transports: _selectedTransports),
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _devices = devices;
      });
    } catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _errorText = error.toString();
      });
    } finally {
      if (mounted) {
        setState(() {
          _loading = false;
        });
      }
    }
  }

  List<UsbTransport> get _selectedTransports {
    return switch (_transportFilter) {
      "mtp" => const <UsbTransport>[UsbTransport.mtp],
      "massStorage" => const <UsbTransport>[UsbTransport.massStorage],
      _ => const <UsbTransport>[],
    };
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text("USB MTP Client Example"),
        actions: <Widget>[
          IconButton(
            onPressed: _loading ? null : _loadDevices,
            icon: const Icon(Icons.refresh),
            tooltip: "Refresh",
          ),
        ],
      ),
      body: Column(
        children: <Widget>[
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Row(
              children: <Widget>[
                Expanded(
                  child: DropdownButtonFormField<String>(
                    initialValue: _transportFilter,
                    decoration: const InputDecoration(
                      labelText: "Transport filter",
                      border: OutlineInputBorder(),
                    ),
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
                      _loadDevices();
                    },
                  ),
                ),
              ],
            ),
          ),
          if (_loading) const LinearProgressIndicator(minHeight: 2),
          if (_errorText != null)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(
                _errorText!,
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              ),
            ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
            child: Row(
              children: <Widget>[
                Text(
                  "Connected devices",
                  style: Theme.of(context).textTheme.titleMedium,
                ),
                const Spacer(),
                Text("${_devices.length} found"),
              ],
            ),
          ),
          Expanded(
            child: _devices.isEmpty
                ? const Center(
                    child: Text("No devices found for the current filter."),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
                    itemCount: _devices.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 12),
                    itemBuilder: (BuildContext context, int index) {
                      final UsbDeviceInfo device = _devices[index];
                      return Card(
                        child: ListTile(
                          title: Text(device.productName ?? device.id),
                          subtitle: Text(_deviceSubtitle(device)),
                          trailing: const Icon(Icons.chevron_right),
                          onTap: () {
                            Navigator.of(context).push(
                              MaterialPageRoute<void>(
                                builder: (_) =>
                                    DeviceBrowserScreen(device: device),
                              ),
                            );
                          },
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }

  String _deviceSubtitle(UsbDeviceInfo device) {
    final List<String> parts = <String>[];
    if (device.manufacturerName != null &&
        device.manufacturerName!.trim().isNotEmpty) {
      parts.add(device.manufacturerName!);
    }
    if (device.vendorId != null) {
      parts.add("VID ${device.vendorId!.toRadixString(16).padLeft(4, "0")}");
    }
    if (device.productId != null) {
      parts.add("PID ${device.productId!.toRadixString(16).padLeft(4, "0")}");
    }
    if (device.transports.isNotEmpty) {
      parts.add(device.transports.map((UsbTransport x) => x.name).join(", "));
    }
    return parts.isEmpty ? device.id : parts.join("  |  ");
  }
}

class DeviceBrowserScreen extends StatefulWidget {
  const DeviceBrowserScreen({super.key, required this.device});

  final UsbDeviceInfo device;

  @override
  State<DeviceBrowserScreen> createState() => _DeviceBrowserScreenState();
}

class _DeviceBrowserScreenState extends State<DeviceBrowserScreen> {
  bool _opening = true;
  bool _loadingEntries = false;
  String? _errorText;
  UsbSession? _session;
  String _currentPath = "/";
  List<UsbEntry> _entries = const <UsbEntry>[];

  @override
  void initState() {
    super.initState();
    _openAndLoadRoot();
  }

  @override
  void dispose() {
    final String? sessionId = _session?.sessionId;
    if (sessionId != null) {
      UsbSync.closeSession(sessionId: sessionId);
    }
    super.dispose();
  }

  Future<void> _openAndLoadRoot() async {
    setState(() {
      _opening = true;
      _errorText = null;
    });

    try {
      final UsbSession session = await UsbSync.openDevice(
        deviceId: widget.device.id,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _session = session;
      });
      await _loadEntries("/");
    } catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _errorText = error.toString();
      });
    } finally {
      if (mounted) {
        setState(() {
          _opening = false;
        });
      }
    }
  }

  Future<void> _loadEntries(String path) async {
    final UsbSession? session = _session;
    if (session == null) {
      return;
    }

    setState(() {
      _loadingEntries = true;
      _errorText = null;
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
    } catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _errorText = error.toString();
      });
    } finally {
      if (mounted) {
        setState(() {
          _loadingEntries = false;
        });
      }
    }
  }

  Future<void> _showFilePath(UsbEntry entry) async {
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text("File: ${entry.path}")));
  }

  String _parentPath(String path) {
    if (path == "/" || path.isEmpty) {
      return "/";
    }
    final int slash = path.lastIndexOf("/");
    if (slash <= 0) {
      return "/";
    }
    return path.substring(0, slash);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.device.productName ?? "Device"),
        actions: <Widget>[
          IconButton(
            onPressed: _loadingEntries
                ? null
                : () => _loadEntries(_currentPath),
            icon: const Icon(Icons.refresh),
            tooltip: "Reload path",
          ),
        ],
      ),
      body: Column(
        children: <Widget>[
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  "Current path",
                  style: Theme.of(context).textTheme.labelLarge,
                ),
                const SizedBox(height: 6),
                SelectableText(
                  _currentPath,
                  style: Theme.of(context).textTheme.bodyLarge,
                ),
                const SizedBox(height: 12),
                Wrap(
                  spacing: 12,
                  runSpacing: 12,
                  children: <Widget>[
                    FilledButton.icon(
                      onPressed: _loadingEntries
                          ? null
                          : () => _loadEntries("/"),
                      icon: const Icon(Icons.home),
                      label: const Text("Root"),
                    ),
                    OutlinedButton.icon(
                      onPressed: _loadingEntries || _currentPath == "/"
                          ? null
                          : () => _loadEntries(_parentPath(_currentPath)),
                      icon: const Icon(Icons.arrow_upward),
                      label: const Text("Up"),
                    ),
                  ],
                ),
              ],
            ),
          ),
          if (_opening || _loadingEntries)
            const LinearProgressIndicator(minHeight: 2),
          if (_errorText != null)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(
                _errorText!,
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              ),
            ),
          Expanded(
            child: _opening
                ? const Center(child: CircularProgressIndicator())
                : _entries.isEmpty
                ? const Center(child: Text("No entries at this path."))
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
                    itemCount: _entries.length,
                    separatorBuilder: (_, _) => const Divider(height: 1),
                    itemBuilder: (BuildContext context, int index) {
                      final UsbEntry entry = _entries[index];
                      return ListTile(
                        leading: Icon(
                          entry.isDirectory
                              ? Icons.folder_outlined
                              : Icons.description_outlined,
                        ),
                        title: Text(entry.name),
                        subtitle: SelectableText(entry.path),
                        trailing: entry.isDirectory
                            ? const Icon(Icons.chevron_right)
                            : null,
                        onTap: () {
                          if (entry.isDirectory) {
                            _loadEntries(entry.path);
                            return;
                          }
                          _showFilePath(entry);
                        },
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
