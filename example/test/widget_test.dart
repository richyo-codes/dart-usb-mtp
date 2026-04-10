import 'package:flutter_test/flutter_test.dart';

import 'package:usb_sync_example/main.dart';

void main() {
  testWidgets("renders USB Sync example shell", (WidgetTester tester) async {
    await tester.pumpWidget(const UsbSyncExampleApp());

    expect(find.text("USB Sync Example"), findsOneWidget);
    expect(find.text("Devices"), findsWidgets);
  });
}
