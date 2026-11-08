// Droidmount — Android phones as Finder disks. Menu bar app.
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import SwiftUI
import UserNotifications

@main
struct DroidmountApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    var body: some Scene {
        MenuBarExtra {
            MenuContent(watcher: delegate.watcher, setup: delegate.setup, updater: delegate.updater)
        } label: {
            MenuIcon(watcher: delegate.watcher, setup: delegate.setup)
        }
        .menuBarExtraStyle(.menu)

        Window("Droidmount Setup", id: "setup") {
            SetupView(setup: delegate.setup, watcher: delegate.watcher)
        }
        .windowResizability(.contentSize)
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let watcher = Watcher()
    let setup = Setup()
    let updater = Updater()

    func applicationDidFinishLaunching(_ notification: Notification) {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        setup.refresh()
        // the FSKit probe takes a few seconds; decide afterwards
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [self] in
            if setup.ready {
                watcher.start()
            } else {
                openSetup()
            }
        }
    }

    /// Unmount the phone before quitting, without blocking the main thread: macOS waits
    /// until reply(toApplicationShouldTerminate:) (at most ~10 s, see Watcher.stop).
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard watcher.isRunning else { return .terminateNow }
        watcher.stop { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    func openSetup() {
        NSApp.activate(ignoringOtherApps: true)
        for w in NSApp.windows where w.identifier?.rawValue == "setup" {
            w.makeKeyAndOrderFront(nil)
            return
        }
        NotificationCenter.default.post(name: .openSetup, object: nil)
    }
}

extension Notification.Name {
    static let openSetup = Notification.Name("droidmount.openSetup")
}

// MARK: - menu bar

struct MenuIcon: View {
    @ObservedObject var watcher: Watcher
    @ObservedObject var setup: Setup
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Image(systemName: symbol)
            .onReceive(NotificationCenter.default.publisher(for: .openSetup)) { _ in
                openWindow(id: "setup")
            }
    }

    private var symbol: String {
        switch watcher.phase {
        case .mounted: return "candybarphone"
        case .connecting: return "ellipsis.circle"
        case .failed: return "exclamationmark.triangle"
        default: return setup.ready ? "candybarphone" : "exclamationmark.triangle"
        }
    }
}

struct MenuContent: View {
    @ObservedObject var watcher: Watcher
    @ObservedObject var setup: Setup
    @ObservedObject var updater: Updater
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Text(statusText)
        if case .mounted(_, let url) = watcher.phase {
            Button("Open in Finder") { NSWorkspace.shared.open(url) }
            Button("Eject") { watcher.eject() }
        }
        Divider()
        if !setup.ready {
            Button("Finish setup…") { showSetup() }
        }
        Toggle("Read-only", isOn: $watcher.readOnly)
        Toggle("Start at login", isOn: Binding(
            get: { setup.loginItem == .ok },
            set: { setup.setLoginItem($0) }))
        if watcher.unsentCount > 0 {
            Button("Unsent files (\(watcher.unsentCount))…") {
                NSWorkspace.shared.open(Watcher.unsentDir)
            }
        }
        Divider()
        Button("Check for Updates…") { updater.checkForUpdates() }
            .disabled(!updater.canCheck)
        Button("Setup…") { showSetup() }
        Button("Show log") { NSWorkspace.shared.open(Watcher.logURL) }
        Button("About Droidmount") {
            NSWorkspace.shared.open(URL(string: "https://github.com/TursiThePanda/droidmount")!)
        }
        Button("Quit Droidmount") { NSApp.terminate(nil) }
            .keyboardShortcut("q")
    }

    private func showSetup() {
        NSApp.activate(ignoringOtherApps: true)
        openWindow(id: "setup")
    }

    private var statusText: String {
        switch watcher.phase {
        case .stopped: return setup.ready ? "Starting…" : "Setup needed"
        case .noPhone: return "No phone connected"
        case .connecting(let n): return "\(n): connecting…"
        case .mounted(let n, _): return "\(n): connected" + (watcher.readOnly ? " (read-only)" : "")
        case .ejected(let n): return "\(n): ejected — replug to mount"
        case .failed(let n): return "\(n): could not mount — unlock and replug"
        }
    }
}

// MARK: - setup window

struct SetupView: View {
    @ObservedObject var setup: Setup
    @ObservedObject var watcher: Watcher

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Set up Droidmount").font(.title2).bold()
            Text("Droidmount shows an Android phone in Finder like a USB drive. It needs macFUSE, a free system extension, and your permission to use it.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)

            StepRow(number: 1, title: "Install macFUSE",
                    detail: "Opens the macFUSE installer that comes with Droidmount. macOS asks for your password once.",
                    check: setup.macfuse) {
                Button("Install macFUSE…") { setup.installMacFUSE() }
            }

            StepRow(number: 2, title: "Allow the macFUSE file system extension",
                    detail: "In System Settings → General → Login Items & Extensions, switch the list to **By Category**, open **File System Extensions** ⓘ and turn on **macFUSE** and **macFUSE (local)**. (The switches in the default view do not respond.)",
                    check: setup.macfuse == .ok ? setup.fskit : .unknown) {
                Button("Open System Settings") { setup.openExtensionSettings() }
                    .disabled(setup.macfuse != .ok)
            }

            StepRow(number: 3, title: "Start Droidmount at login",
                    detail: "Recommended, so the phone appears whenever you plug it in.",
                    check: setup.loginItem) {
                Button(setup.loginItem == .ok ? "Turn off" : "Turn on") {
                    setup.setLoginItem(setup.loginItem != .ok)
                }
            }

            Divider()
            HStack {
                if setup.ready {
                    Label("Ready. Plug in your phone and choose “File transfer” on it.",
                          systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                } else {
                    Label("Droidmount keeps checking; the steps tick off by themselves.",
                          systemImage: "clock")
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
        }
        .padding(24)
        .frame(width: 520)
        .onAppear { setup.startPolling() }
        .onDisappear { setup.stopPolling() }
        .onChange(of: setup.ready) { _, ready in
            if ready { watcher.start() }
        }
    }
}

struct StepRow<Actions: View>: View {
    let number: Int
    let title: String
    let detail: LocalizedStringKey
    let check: Setup.Check
    @ViewBuilder let actions: () -> Actions

    init(number: Int, title: String, detail: String, check: Setup.Check,
         @ViewBuilder actions: @escaping () -> Actions) {
        self.number = number
        self.title = title
        self.detail = LocalizedStringKey(detail)
        self.check = check
        self.actions = actions
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            statusIcon.font(.title3).frame(width: 24)
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.headline)
                Text(detail).fixedSize(horizontal: false, vertical: true)
                    .foregroundStyle(.secondary)
                if check != .ok { HStack { actions() } }
            }
        }
    }

    @ViewBuilder private var statusIcon: some View {
        switch check {
        case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .checking: ProgressView().controlSize(.small)
        case .missing: Image(systemName: "\(number).circle").foregroundStyle(.orange)
        case .unknown: Image(systemName: "\(number).circle").foregroundStyle(.secondary)
        }
    }
}
