//
//  DoorShortcuts.swift
//  door
//
//  Registers the "Open Door" action with the Shortcuts app and Siri so it
//  shows up automatically once the app is installed — no manual setup.
//

import AppIntents

struct DoorShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: OpenDoorIntent(),
            phrases: [
                "Open the door with \(.applicationName)",
                "\(.applicationName) open the door",
            ],
            shortTitle: "Open Door",
            systemImageName: "door.left.hand.open"
        )
    }
}
