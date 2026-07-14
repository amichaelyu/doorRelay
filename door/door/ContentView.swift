//
//  ContentView.swift
//  door
//
//  One big "Open Door" button. Reuses the same OpenDoorIntent that Shortcuts
//  and the widget use, so behaviour is identical everywhere.
//

import SwiftUI

struct ContentView: View {
    @State private var state: OpenState = .idle

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

            Text(statusText)
                .font(.headline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
                .frame(minHeight: 44)

            Spacer()

            Button(action: open) {
                HStack(spacing: 10) {
                    if state == .opening {
                        ProgressView()
                            .tint(.white)
                    }
                    Text(state == .opening ? "Opening…" : "Open Door")
                        .font(.title2.weight(.bold))
                }
                .frame(maxWidth: .infinity)
                .frame(height: 64)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(state == .opening)
            .padding(.horizontal, 24)
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
        default: return .accentColor
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
