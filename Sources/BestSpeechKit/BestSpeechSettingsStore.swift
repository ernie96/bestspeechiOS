import Foundation

public final class BestSpeechSettingsStore {
    public static let groupIdentifier = "group.com.bestspeech"
    public static let settingsKey = "BestSpeechSavedSettings"

    private static var fileURL: URL? {
        return containerURL?.appendingPathComponent(settingsKey + ".json")
    }

    public static var containerURL: URL? {
        if let url = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupIdentifier) {
            return url
        }
        
        let mainBundle = Bundle.main
        let appBundle = Bundle(url: mainBundle.bundleURL.deletingLastPathComponent().deletingLastPathComponent())
        
        let paths = [
            mainBundle.path(forResource: "embedded", ofType: "mobileprovision"),
            appBundle?.path(forResource: "embedded", ofType: "mobileprovision")
        ].compactMap { $0 }
        
        for path in paths {
            if let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
               let string = String(data: data, encoding: .isoLatin1),
               let groupsRange = string.range(of: "<key>com.apple.security.application-groups</key>") {
                let tail = string[groupsRange.upperBound...]
                if let arrayStart = tail.range(of: "<array>"), let arrayEnd = tail.range(of: "</array>") {
                    let arrayContent = tail[arrayStart.upperBound..<arrayEnd.lowerBound]
                    if let stringStart = arrayContent.range(of: "<string>"), let stringEnd = arrayContent.range(of: "</string>") {
                        let dynamicAppGroup = String(arrayContent[stringStart.upperBound..<stringEnd.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
                        if let url = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: dynamicAppGroup) {
                            return url
                        }
                    }
                }
            }
        }
        
        return FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
    }

    public static func load() -> BestSpeechSettings {
        guard let url = fileURL,
              let data = try? Data(contentsOf: url),
              let settings = try? JSONDecoder().decode(BestSpeechSettings.self, from: data) else {
            return .default
        }
        return settings
    }

    public static func save(_ settings: BestSpeechSettings) {
        if let url = fileURL, let data = try? JSONEncoder().encode(settings) {
            try? data.write(to: url, options: .atomic)
        }
    }
}
