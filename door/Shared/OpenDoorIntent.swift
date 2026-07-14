//
//  OpenDoorIntent.swift
//  Shared between the app and the widget extension.
//
//  The single App Intent that opens the door. It powers three things:
//   - the Shortcuts app (exposed via DoorShortcuts.AppShortcutsProvider),
//   - the interactive widget button (Button(intent:) in DoorWidget),
//   - "Hey Siri, open the door with door".
//

import AppIntents

struct OpenDoorIntent: AppIntent {
    static var title: LocalizedStringResource = "Open Door"
    static var description = IntentDescription("Pulses the door relay to open the door.")

    // Run in the background — never bounce the user into the app. This is what
    // lets the widget button and Shortcuts fire the relay without launching.
    static var openAppWhenRun: Bool = false

    func perform() async throws -> some IntentResult & ProvidesDialog {
        try await DoorService.open()
        return .result(dialog: "🚪 Door opened.")
    }
}
