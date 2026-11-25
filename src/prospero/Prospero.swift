import AppKit
import SwiftUI
import UniformTypeIdentifiers

private enum VirtualRefreshRate: Int {
    case hz60 = 60
    case hz120 = 120

    static let standard = Self.hz60
}

struct LibraryGame: Codable, Identifiable, Hashable {
    let path: String
    let title: String
    var patch: String?
    var id: String { path }

    init(url: URL) {
        let folder = url.lastPathComponent == "eboot.bin" ? url.deletingLastPathComponent() : url
        path = folder.standardizedFileURL.path
        let manifest = folder.appendingPathComponent("sce_sys/param.json")
        if let data = try? Data(contentsOf: manifest),
           let fields = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let localization = fields["localizedParameters"] as? [String: Any],
           let locale = localization["defaultLanguage"] as? String,
           let entry = localization[locale] as? [String: Any],
           let name = entry["titleName"] as? String, !name.isEmpty {
            title = name
        } else {
            title = folder.deletingPathExtension().lastPathComponent
        }
    }

    var isPackage: Bool {
        ["pkg", "fpkg"].contains(URL(fileURLWithPath: path).pathExtension.lowercased())
    }
    var artwork: NSImage? {
        NSImage(contentsOfFile: URL(fileURLWithPath: path).appendingPathComponent("sce_sys/icon0.png").path)
    }
}

@MainActor
final class Library: ObservableObject {
    @Published var games: [LibraryGame] = []
    @Published var selection: String?
    @Published var running: String?
    @Published var error: String?
    private var emulatorProcess: Process?

    init() {
        UserDefaults.standard.register(defaults: [
            "tessellation": true,
            "window1080": true,
            "virtualRefreshRate": VirtualRefreshRate.standard.rawValue
        ])
        if let data = UserDefaults.standard.data(forKey: "games"),
           let decoded = try? JSONDecoder().decode([LibraryGame].self, from: data) {
            games = decoded
        }
    }

    func showAddGames() {
        let panel = NSOpenPanel()
        panel.message = "Choose a game folder, eboot.bin, or a supported game package."
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        if panel.runModal() == .OK { addGames(panel.urls) }
    }

    func addGames(_ urls: [URL]) {
        var rejected: [String] = []
        var firstAdded: String?
        for url in urls {
            let game = LibraryGame(url: url)
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: game.path, isDirectory: &isDirectory),
                  (game.isPackage && !isDirectory.boolValue) || (isDirectory.boolValue &&
                      FileManager.default.fileExists(atPath: game.path + "/eboot.bin")) else {
                rejected.append(url.lastPathComponent)
                continue
            }
            if !games.contains(where: { $0.id == game.id }) { games.append(game) }
            if firstAdded == nil { firstAdded = game.id }
        }
        if let firstAdded { selection = firstAdded }
        save()
        if !rejected.isEmpty {
            error = "Choose a .pkg or .fpkg file or a game folder containing eboot.bin. Could not add "
                + rejected.joined(separator: ", ") + "."
        }
    }

    func choosePatch(for game: LibraryGame) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        setPatch(url.path, for: game)
    }

    func setPatch(_ path: String?, for game: LibraryGame) {
        guard let index = games.firstIndex(where: { $0.id == game.id }) else { return }
        games[index].patch = path
        save()
    }

    func remove(_ game: LibraryGame) {
        games.removeAll { $0.id == game.id }
        save()
    }

    private func save() {
        games.sort { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
        if let data = try? JSONEncoder().encode(games) {
            UserDefaults.standard.set(data, forKey: "games")
        }
    }

    static func packageReader(override: String, emulator: URL) -> String? {
        if !override.isEmpty {
            return FileManager.default.isExecutableFile(atPath: override) ? override : nil
        }
        let coreFolder = emulator.deletingLastPathComponent()
        let candidates = [
            Bundle.main.url(forResource: "read-fpkg", withExtension: nil),
            coreFolder.appendingPathComponent("read-fpkg"),
            coreFolder.deletingLastPathComponent().appendingPathComponent("fpkg-reader/read-fpkg")
        ]
        return candidates.compactMap { $0?.path }.first {
            FileManager.default.isExecutableFile(atPath: $0)
        }
    }

    func launch(_ game: LibraryGame) {
        guard emulatorProcess == nil else { return }
        guard FileManager.default.fileExists(atPath: game.path) else {
            error = "The game is unavailable. Connect its drive and try again."
            return
        }
        let defaults = UserDefaults.standard
        let core = defaults.string(forKey: "core") ?? ""
        let reader = defaults.string(forKey: "reader") ?? ""
        let refreshRate = VirtualRefreshRate(rawValue: defaults.integer(forKey: "virtualRefreshRate"))
            ?? .standard
        let corePath = core.isEmpty ? Bundle.main.bundleURL.deletingLastPathComponent()
            .appendingPathComponent("kyty_emulator").path : core
        guard FileManager.default.isExecutableFile(atPath: corePath) else {
            error = "Select the kyty_emulator executable in Prospero Settings."
            return
        }
        let executable = URL(fileURLWithPath: corePath)
        let resolvedReader = game.isPackage ? Self.packageReader(override: reader, emulator: executable) : nil
        if game.isPackage && resolvedReader == nil {
            error = "Select a compatible package reader in Prospero Settings."
            return
        }
        if let patch = game.patch, !FileManager.default.isReadableFile(atPath: patch) {
            error = "The selected game patch is unavailable."
            return
        }
        let process = Process()
        process.executableURL = executable
        process.currentDirectoryURL = executable.deletingLastPathComponent()
        var environment = ProcessInfo.processInfo.environment
        environment["KYTY_BACKEND"] = "metal"
        environment["KYTY_MSL_ONLY"] = "1"
        environment["KYTY_MSL_GDS"] = "1"
        environment["KYTY_VIDEO_OUT_1080P"] = defaults.bool(forKey: "window1080") ? "1" : nil
        environment["KYTY_FPKG_READER"] = resolvedReader
        process.environment = environment
        var arguments = ["--game", game.path, "--screen-width", "1920", "--screen-height", "1080",
                         "--vblank-frequency", String(refreshRate.rawValue)]
        if defaults.bool(forKey: "tessellation") { arguments += ["--tessellation"] }
        if let patch = game.patch { arguments += ["--game-patch", patch] }
        process.arguments = arguments
        process.terminationHandler = { [weak self] ended in
            let status = ended.terminationStatus
            let reason = ended.terminationReason
            Task { @MainActor in
                self?.emulatorProcess = nil
                self?.running = nil
                if reason == .exit && status != 0 {
                    self?.error = "The emulator exited with status \(status)."
                }
            }
        }
        do {
            try process.run()
            emulatorProcess = process
            running = game.id
        } catch {
            self.error = error.localizedDescription
        }
    }
}

struct LibraryView: View {
    @ObservedObject var library: Library

    var body: some View {
        NavigationSplitView {
            List(selection: $library.selection) {
                ForEach(library.games) { game in
                    Label(game.title, systemImage: "gamecontroller")
                        .tag(game.id)
                        .contextMenu {
                            Button("Remove from Library") { library.remove(game) }
                        }
                }
            }
            .dropDestination(for: URL.self) { urls, _ in
                library.addGames(urls)
                return true
            }
            .navigationTitle("Library")
            .navigationSplitViewColumnWidth(min: 190, ideal: 240)
            .toolbar {
                Button("Add Games", systemImage: "plus") { library.showAddGames() }
                SettingsLink { Label("Settings", systemImage: "gearshape") }
            }
        } detail: {
            if let game = library.games.first(where: { $0.id == library.selection }) {
                VStack(alignment: .leading, spacing: 20) {
                    if let artwork = game.artwork {
                        Image(nsImage: artwork).resizable().aspectRatio(contentMode: .fit)
                            .frame(maxHeight: 220).clipShape(.rect(cornerRadius: 12))
                    } else {
                        Image(systemName: "gamecontroller.fill").font(.system(size: 58))
                            .foregroundStyle(.tint).accessibilityHidden(true)
                    }
                    Text(game.title).font(.largeTitle.bold())
                    Text(game.path).font(.callout).foregroundStyle(.secondary)
                        .textSelection(.enabled)
                    if let patch = game.patch {
                        Text(URL(fileURLWithPath: patch).lastPathComponent)
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Button(library.running == game.id ? "Running" : "Play", systemImage: "play.fill") {
                        library.launch(game)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(library.running != nil)
                    Menu("Game Options") {
                        Button("Choose Patch…") { library.choosePatch(for: game) }
                        if game.patch != nil {
                            Button("Clear Patch") { library.setPatch(nil, for: game) }
                        }
                    }
                    .fixedSize()
                }
                .padding(36)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
            } else {
                ContentUnavailableView {
                    Label(library.games.isEmpty ? "Your Library Is Empty" : "Select a Game",
                          systemImage: "gamecontroller")
                } actions: {
                    if library.games.isEmpty {
                        Button("Add Games") { library.showAddGames() }.buttonStyle(.borderedProminent)
                    }
                }
            }
        }
        .frame(minWidth: 720, minHeight: 430)
        .alert("Prospero", isPresented: Binding(get: { library.error != nil },
                                               set: { if !$0 { library.error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: { Text(library.error ?? "") }
    }
}

struct ProsperoSettings: View {
    @AppStorage("core") private var core = ""
    @AppStorage("reader") private var reader = ""
    @AppStorage("tessellation") private var tessellation = true
    @AppStorage("window1080") private var window1080 = true
    @AppStorage("virtualRefreshRate") private var virtualRefreshRate = VirtualRefreshRate.standard
    @AppStorage("appearance") private var appearance = "system"

    var body: some View {
        Form {
            Section("Emulator") {
                FileSetting(title: "Executable", path: $core, defaultLabel: "Use adjacent emulator")
                FileSetting(title: "Package reader", path: $reader, defaultLabel: "Find adjacent reader")
                Toggle("Enable tessellation", isOn: $tessellation)
            }
            Section("Video") {
                Toggle("Report 1080p to games", isOn: $window1080)
                Picker("Virtual refresh rate", selection: $virtualRefreshRate) {
                    Text("60 Hz").tag(VirtualRefreshRate.hz60)
                    Text("120 Hz").tag(VirtualRefreshRate.hz120)
                }
                Text("Games choose their internal render resolution.")
                    .foregroundStyle(.secondary)
            }
            Picker("Appearance", selection: $appearance) {
                Text("System").tag("system")
                Text("Light").tag("light")
                Text("Dark").tag("dark")
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 510, minHeight: 330)
    }
}

struct FileSetting: View {
    var title: String
    @Binding var path: String
    var defaultLabel = "None"

    var body: some View {
        LabeledContent(title) {
            Text(path.isEmpty ? defaultLabel : URL(fileURLWithPath: path).lastPathComponent)
                .lineLimit(1).help(path)
            Button("Choose…") {
                let panel = NSOpenPanel()
                if panel.runModal() == .OK, let file = panel.url { path = file.path }
            }
            if !path.isEmpty {
                Button("Clear") { path = "" }
                    .accessibilityLabel("Clear \(title)")
            }
        }
    }
}

@MainActor
final class ProsperoAppDelegate: NSObject, NSApplicationDelegate {
    let library = Library()

    func application(_ application: NSApplication, open urls: [URL]) {
        library.addGames(urls)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let arguments = CommandLine.arguments
        guard let play = arguments.firstIndex(of: "--play"), play + 1 < arguments.count else { return }
        let url = URL(fileURLWithPath: arguments[play + 1])
        library.addGames([url])
        let identifier = LibraryGame(url: url).id
        guard var game = library.games.first(where: { $0.id == identifier }) else { return }
        if let patch = arguments.firstIndex(of: "--game-patch"), patch + 1 < arguments.count {
            let patchPath = URL(fileURLWithPath: arguments[patch + 1]).standardizedFileURL.path
            library.setPatch(patchPath, for: game)
            game.patch = patchPath
        }
        library.launch(game)
    }
}

@main
struct ProsperoApp: App {
    @NSApplicationDelegateAdaptor(ProsperoAppDelegate.self) private var delegate
    @AppStorage("appearance") private var appearance = "system"

    var body: some Scene {
        WindowGroup("Prospero") {
            LibraryView(library: delegate.library)
                .preferredColorScheme(appearance == "light" ? .light : appearance == "dark" ? .dark : nil)
        }
        Settings { ProsperoSettings() }
    }
}
