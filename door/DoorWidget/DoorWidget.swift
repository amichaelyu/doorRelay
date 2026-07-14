//
//  DoorWidget.swift
//  DoorWidget
//
//  A Home Screen widget that is a single interactive button. Tapping it runs
//  OpenDoorIntent directly in the widget process (openAppWhenRun == false), so
//  the door opens without launching the app.
//

import WidgetKit
import SwiftUI
import AppIntents

struct DoorEntry: TimelineEntry {
    let date: Date
}

struct DoorProvider: TimelineProvider {
    func placeholder(in context: Context) -> DoorEntry {
        DoorEntry(date: Date())
    }

    func getSnapshot(in context: Context, completion: @escaping (DoorEntry) -> Void) {
        completion(DoorEntry(date: Date()))
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<DoorEntry>) -> Void) {
        // Static button — no need to ever refresh the timeline.
        completion(Timeline(entries: [DoorEntry(date: Date())], policy: .never))
    }
}

struct DoorWidgetEntryView: View {
    var entry: DoorEntry

    var body: some View {
        Button(intent: OpenDoorIntent()) {
            VStack(spacing: 6) {
                Image(systemName: "door.left.hand.open")
                    .font(.title)
                Text("Open Door")
                    .font(.headline)
                    .minimumScaleFactor(0.7)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .buttonStyle(.plain)
        .foregroundStyle(.white)
        .containerBackground(for: .widget) {
            LinearGradient(
                colors: [.accentColor, .accentColor.opacity(0.7)],
                startPoint: .top,
                endPoint: .bottom
            )
        }
    }
}

struct DoorWidget: Widget {
    let kind = "DoorWidget"

    var body: some WidgetConfiguration {
        StaticConfiguration(kind: kind, provider: DoorProvider()) { entry in
            DoorWidgetEntryView(entry: entry)
        }
        .configurationDisplayName("Open Door")
        .description("A button that opens the door.")
        .supportedFamilies([.systemSmall, .systemMedium])
    }
}
