// Droidmount — first-run checks: macFUSE installed, its FSKit module enabled, login item.
// Every step the user must do is opened for them and detected when it is done.
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import Foundation
import ServiceManagement

@MainActor
final class Setup: ObservableObject {
    enum Check: Equatable { case unknown, checking, ok, missing }

    @Published private(set) var macfuse: Check = .unknown
    @Published private(set) var fskit: Check = .unknown
    @Published private(set) var loginItem: Check = .unknown
    var ready: Bool { macfuse == .ok && fskit == .ok }

    static let macfuseBundle = URL(fileURLWithPath: "/Library/Filesystems/macfuse.fs")
    static let macfuseApp = macfuseBundle.appendingPathComponent("Contents/Resources/macfuse.app")

    private var probing = false
    private var registeredModules = false
    private var timer: Timer?

    /// Re-check every 2 s while the setup window is open.
    func startPolling() {
        refresh()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    func refresh() {
        macfuse = FileManager.default.fileExists(atPath: Self.macfuseBundle.path) ? .ok : .missing
        loginItem = SMAppService.mainApp.status == .enabled ? .ok : .missing
        if macfuse == .ok && fskit != .ok && !probing {
            if !registeredModules {
                registerModules()
            }
            probeFSKit()
        } else if macfuse != .ok {
            fskit = .unknown
        }
    }

    // MARK: actions

    func installMacFUSE() {
        guard let pkg = Bundle.main.url(forResource: "Install macFUSE", withExtension: "pkg") else {
            NSWorkspace.shared.open(URL(string: "https://macfuse.github.io/")!)
            return
        }
        NSWorkspace.shared.open(pkg)
    }

    /// macFUSE's FSKit modules only appear in System Settings after macfuse.app ran once.
    func registerModules() {
        registeredModules = true
        guard FileManager.default.fileExists(atPath: Self.macfuseApp.path) else { return }
        let cfg = NSWorkspace.OpenConfiguration()
        cfg.activates = false
        cfg.hides = true
        NSWorkspace.shared.openApplication(at: Self.macfuseApp, configuration: cfg)
    }

    func openExtensionSettings() {
        registerModules()
        let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!
        NSWorkspace.shared.open(url)
    }

    func setLoginItem(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            NSLog("Droidmount: login item: \(error)")
        }
        loginItem = SMAppService.mainApp.status == .enabled ? .ok : .missing
    }

    // MARK: probe

    /// Mount an empty read-only filesystem through macFUSE's FSKit backend. If it appears,
    /// the module is enabled. pluginkit does not report that state.
    private func probeFSKit() {
        probing = true
        if fskit != .ok { fskit = .checking }
        let probe = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/fskit-probe")
        Task.detached {
            let ok = Self.runProbe(probe)
            await MainActor.run {
                self.fskit = ok ? .ok : .missing
                self.probing = false
            }
        }
    }

    nonisolated private static func runProbe(_ probe: URL) -> Bool {
        let mp = "/Volumes/DroidmountProbe"
        let p = Process()
        p.executableURL = probe
        p.arguments = [mp]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        var ok = false
        for _ in 0..<32 { // up to 8 s
            if isMountPoint(mp) { ok = true; break }
            if !p.isRunning { break }
            Thread.sleep(forTimeInterval: 0.25)
        }
        let um = Process()
        um.executableURL = URL(fileURLWithPath: "/sbin/umount")
        um.arguments = ["-f", mp]
        um.standardError = FileHandle.nullDevice
        try? um.run()
        um.waitUntilExit()
        if p.isRunning { p.terminate() }
        return ok
    }

    nonisolated private static func isMountPoint(_ path: String) -> Bool {
        var a = stat(), b = stat()
        guard stat(path, &a) == 0, stat(path + "/..", &b) == 0 else { return false }
        return a.st_dev != b.st_dev
    }
}
