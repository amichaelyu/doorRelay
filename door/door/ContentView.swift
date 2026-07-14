//
//  ContentView.swift
//  door
//
//  One big "Open Door" button. Reuses the same OpenDoorIntent that Shortcuts
//  and the widget use, so behaviour is identical everywhere.
//

import SwiftUI

struct ContentView: View {
    @Environment(\.openURL) private var openURL
    @Environment(\.colorScheme) private var colorScheme
    @State private var state: OpenState = .idle

    /// Ready-made "Open Door" shortcut, prebuilt on the app's Open Door
    /// App Intent so it runs through the same API path (see config.yaml
    /// `setup.shortcut_url`). Tapping installs it in one step.
    private let shortcutURL = URL(string: "https://www.icloud.com/shortcuts/50603687740c4b1cbc30c12a2030c509")!

    enum OpenState: Equatable {
        case idle
        case opening
        case opened
        case failed(String)
    }

    var body: some View {
        VStack(spacing: 28) {
            Spacer()

            Image(systemName: iconName)
                .font(.system(size: 88))
                .foregroundStyle(iconColor)
                .contentTransition(.symbolEffect(.replace))
                .animation(.snappy, value: state)
                .onTapGesture(perform: open)
                .accessibilityAddTraits(.isButton)
                .accessibilityLabel("Open Door")
                .disabled(state == .opening)

            Text(statusText)
                .font(.headline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
                .frame(minHeight: 44)

            Spacer()

            Button {
                openURL(shortcutURL)
            } label: {
                Label("Add “Open Door” Shortcut", systemImage: "plus.app")
                    .font(.callout)
            }
            .padding(.bottom, 40)
        }
    }

    private func open() {
        state = .opening
        Task {
            do {
                try await DoorService.open()
                state = .opened
                try? await Task.sleep(for: .seconds(2))
                if state == .opened { state = .idle }
            } catch {
                state = .failed(error.localizedDescription)
            }
        }
    }

    private var iconName: String {
        switch state {
        case .idle, .opening: return "door.left.hand.closed"
        case .opened: return "door.left.hand.open"
        case .failed: return "exclamationmark.triangle"
        }
    }

    private var iconColor: Color {
        switch state {
        case .opened: return .green
        case .failed: return .orange
        // Explicit per-appearance tint for the resting door icon so it stays
        // legible in both modes: a warm brown in light mode, a lighter tan in
        // dark mode. `colorScheme` tracks the system Light/Dark setting.
        default: return colorScheme == .dark
            ? Color(red: 0.82, green: 0.60, blue: 0.44)
            : Color(red: 0.55, green: 0.33, blue: 0.18)
        }
    }

    private var statusText: String {
        switch state {
        case .idle: return "Tap to open the door."
        case .opening: return "Reaching the door relay…"
        case .opened: return "Door opened."
        case .failed(let message): return message
        }
    }
}

#Preview {
    ContentView()
}
