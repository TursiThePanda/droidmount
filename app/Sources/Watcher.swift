// Droidmount — runs the android-automount helper and turns its event lines into state.
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import Foundation
import UserNotifications

@MainActor
final class Watcher: ObservableObject {
    enum Phase: Equatable {
        case stopped            // helper not running (setup incomplete or quitting)
        case noPhone
        case connecting(String)
        case mounted(String, URL)
        case ejected(String)
        case failed(String)     // gave up; replug the phone
    }

    @Published private(set) var phase: Phase = .stopped
    @Published private(set) var unsentCount = 0
    @Published var readOnly: Bool {
        didSet {
            UserDefaults.standard.set(readOnly, forKey: "readOnly")
            if oldValue != readOnly { restart() }
        }
    }

    static let unsentDir = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/Droidmount/unsent")

    private var process: Process?
    private var buffer = Data()
    private var stopping = false
    private var stopInProgress = false
    private var stopWaiters: [() -> Void] = []

    init() {
        readOnly = UserDefaults.standard.bool(forKey: "readOnly")
        refreshUnsent()
    }

    private var helpers: URL { Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers") }

    func start() {
        guard process == nil else { return }
        stopping = false
        let p = Process()
        p.executableURL = helpers.appendingPathComponent("android-automount")
        var args = [helpers.appendingPathComponent("simple-mtpfs").path]
        if readOnly { args.append("ro") }
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["DROIDMOUNT_APP"] = "1"
        p.environment = env

        let out = Pipe()
        p.standardOutput = out
        // O_APPEND: FileHandle(forWritingAtPath:) starts at offset 0, so every launch
        // overwrote the start of the log and left older runs' lines behind it.
        let logFD = open(Self.logURL.path, O_WRONLY | O_APPEND | O_CREAT, 0o644)
        p.standardError = logFD >= 0 ? FileHandle(fileDescriptor: logFD, closeOnDealloc: true)
                                     : FileHandle.nullDevice
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            Task { @MainActor in self?.consume(data) }
        }
        p.terminationHandler = { [weak self] ended in
            Task { @MainActor in self?.helperEnded(ended) }
        }
        do {
            try p.run()
            process = p
            phase = .noPhone
        } catch {
            phase = .failed("Could not start the helper: \(error.localizedDescription)")
        }
    }

    var isRunning: Bool { process?.isRunning ?? false }

    /// Unmounts (the helper does it on SIGTERM) and stops. Never blocks the main thread:
    /// waiting with waitUntilExit() on the main thread hung the app on Quit (the exit of
    /// the helper was never noticed), so completion runs once the helper has gone - or
    /// after 10 s, when it is killed outright.
    func stop(then completion: (() -> Void)? = nil) {
        stopping = true
        if let completion { stopWaiters.append(completion) }
        guard let p = process, p.isRunning else {
            finishStop()
            return
        }
        guard !stopInProgress else { return }
        stopInProgress = true
        let pid = p.processIdentifier
        p.terminate()
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
            guard let self, self.stopInProgress, self.process === p else { return }
            kill(pid, SIGKILL)
            self.finishStop()
        }
    }

    private func finishStop() {
        stopInProgress = false
        process = nil
        phase = .stopped
        let waiters = stopWaiters
        stopWaiters = []
        waiters.forEach { $0() }
    }

    func restart() {
        guard process != nil else { return }
        stop { [weak self] in self?.start() }
    }

    func eject() {
        guard let p = process else { return }
        kill(p.processIdentifier, SIGUSR1)
    }

    func refreshUnsent() {
        let items = (try? FileManager.default.contentsOfDirectory(atPath: Self.unsentDir.path)) ?? []
        unsentCount = items.filter { !$0.hasPrefix(".") }.count
    }

    static var logURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/Droidmount.log")
    }

    // MARK: - events from the helper

    private func consume(_ data: Data) {
        if data.isEmpty { return }
        buffer.append(data)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self)
            buffer.removeSubrange(buffer.startIndex...nl)
            handle(line)
        }
    }

    private func handle(_ line: String) {
        guard line.hasPrefix("@") else { return }
        let body = line.dropFirst()
        let (verb, rest) = split(String(body))
        switch verb {
        case "watching":
            phase = .noPhone
        case "plugged", "mounting":
            let name = rest.components(separatedBy: "\t").first ?? rest
            if case .mounted = phase { break }
            phase = .connecting(name)
        case "mounted":
            let parts = rest.components(separatedBy: "\t")
            if parts.count == 2 { phase = .mounted(parts[0], URL(fileURLWithPath: parts[1])) }
        case "unmounted":
            if case .mounted(let name, _) = phase { phase = .connecting(name) }
        case "ejected":
            phase = .ejected(rest)
        case "gone":
            phase = .noPhone
        case "giveup":
            phase = .failed(rest)
            notify(title: "Could not open \(rest)",
                   body: "Unlock the phone, choose File transfer, then plug it in again.")
        case "hint":
            let (kind, name) = split(rest)
            if kind == "filetransfer" {
                notify(title: "\(name) is connected",
                       body: "To see it in Finder, choose “File transfer” in the phone's USB notification.")
            } else if kind == "unlock" {
                notify(title: "Unlock \(name)",
                       body: "Android only shares its files while the phone is unlocked.")
            }
        case "unsent":
            let name = rest.components(separatedBy: "\t").first ?? rest
            refreshUnsent()
            notify(title: "\(name) was not copied to the phone",
                   body: "Nothing was lost: a copy is in Droidmount's “Unsent files”.")
        default:
            break
        }
    }

    private func helperEnded(_ ended: Process) {
        guard process === ended else { return } // a late notice about an earlier helper
        if stopping {
            finishStop()
            return
        }
        // crashed or killed: come back after a moment
        process = nil
        phase = .noPhone
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.start() }
    }

    private func split(_ s: String) -> (String, String) {
        guard let sp = s.firstIndex(of: " ") else { return (s, "") }
        return (String(s[..<sp]), String(s[s.index(after: sp)...]))
    }

    private func notify(title: String, body: String) {
        let c = UNMutableNotificationContent()
        c.title = title
        c.body = body
        let req = UNNotificationRequest(identifier: UUID().uuidString, content: c, trigger: nil)
        UNUserNotificationCenter.current().add(req)
    }
}
