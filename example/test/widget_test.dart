import 'package:flutter_test/flutter_test.dart';

import 'package:usb_mtp_client_example/main.dart';

void main() {
  testWidgets("renders USB Sync example shell", (WidgetTester tester) async {
    await tester.pumpWidget(const UsbSyncExampleApp());

    expect(find.text("USB MTP Client Example"), findsOneWidget);
    expect(find.text("Connected devices"), findsOneWidget);
  });
}
