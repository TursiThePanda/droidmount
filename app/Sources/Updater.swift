// Droidmount — automatic updates with Sparkle.
// The feed (SUFeedURL) is the appcast.xml attached to the latest GitHub release; every
// update must carry an EdDSA signature matching SUPublicEDKey, so a tampered download is
// refused. Sparkle asks the user once whether to check automatically (daily).
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import Sparkle

@MainActor
final class Updater: NSObject, ObservableObject, SPUStandardUserDriverDelegate {
    private(set) var controller: SPUStandardUpdaterController!
    @Published private(set) var canCheck = false
    private var observation: NSKeyValueObservation?

    override init() {
        super.init()
        controller = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil,
                                                  userDriverDelegate: self)
        observation = controller.updater.observe(\.canCheckForUpdates, options: [.initial, .new]) {
            [weak self] updater, _ in
            let can = updater.canCheckForUpdates
            Task { @MainActor in self?.canCheck = can }
        }
    }

    func checkForUpdates() {
        NSApp.activate(ignoringOtherApps: true) // menu bar app: bring Sparkle's window forward
        controller.checkForUpdates(nil)
    }

    // A menu bar app has no Dock icon to draw attention, so let Sparkle show scheduled
    // update alerts gently instead of stealing focus.
    nonisolated var supportsGentleScheduledUpdateReminders: Bool { true }
}
