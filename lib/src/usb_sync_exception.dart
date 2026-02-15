class UsbSyncException implements Exception {
  final String code;
  final String message;
  final Object? details;

  const UsbSyncException({
    required this.code,
    required this.message,
    this.details,
  });

  @override
  String toString() {
    return "UsbSyncException($code): $message";
  }
}
