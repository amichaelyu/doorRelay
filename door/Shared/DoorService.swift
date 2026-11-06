//
//  DoorService.swift
//  Shared between the app and the widget extension.
//
//  One place that knows how to talk to the door opener HTTP API
//  (see door_monitor_api.py). It POSTs to /open with the shared bearer
//  token — the same contract the Cloudflare Worker uses.
//

import Foundation

enum DoorService {
    /// Public endpoint fronted by the Cloudflare Worker at door.yumichael.com.
    static let openURL = URL(string: "https://door.yumichael.com/open")!

    /// Shared API token: DOOR_API_TOKEN from the repo-root .env, copied into
    /// Info.plist (DoorAPIToken) at build time by embed-env.sh. NOTE: it still
    /// ships inside the app bundle — anyone who extracts the build can open the
    /// door. If it leaks, rotate it in .env and rebuild.
    static let token = Bundle.main.object(forInfoDictionaryKey: "DoorAPIToken") as? String ?? ""

    struct DoorError: LocalizedError {
        var errorDescription: String? { "Couldn't connect to door." }
    }

    /// Pulse the relay. Returns normally on success; on any transport error or
    /// non-2xx response throws a single friendly "Couldn't connect to door."
    static func open() async throws {
        var request = URLRequest(url: openURL)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.timeoutInterval = 15

        do {
            let (_, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode) else {
                throw DoorError()
            }
        } catch is DoorError {
            throw DoorError()
        } catch {
            // Wrap URLSession/transport errors in the same friendly message.
            throw DoorError()
        }
    }
}
