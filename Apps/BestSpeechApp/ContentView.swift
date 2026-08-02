import SwiftUI
import AVFoundation

struct ContentView: View {
    @State private var settings = BestSpeechSettingsStore.load()
    @State private var testText: String = "Hello, this is a test of BestSpeech, the 32-bit synthesizer."
    @State private var synthesizer: BestSpeechSynthesizer? = nil
    @State private var audioPlayer: AVAudioPlayer? = nil
    @State private var statusMessage: String = ""

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Test Speech")) {
                    TextField("Enter text to speak", text: $testText)
                    Button("Play Speech") {
                        playTestSpeech()
                    }
                    if !statusMessage.isEmpty {
                        Text(statusMessage)
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                
                Section(header: Text("Engine Parameters")) {
                    VStack(alignment: .leading) {
                        Text("Speed multiplier: \(settings.rate, specifier: "%.2f")")
                        Slider(value: $settings.rate, in: 0.5...4.0, step: 0.1) { _ in
                            saveSettings()
                        }
                    }
                    VStack(alignment: .leading) {
                        Text("Pitch: \(settings.pitch)%")
                        Slider(value: Binding(
                            get: { Float(settings.pitch) },
                            set: { settings.pitch = Int32($0); saveSettings() }
                        ), in: 43...600, step: 1)
                    }
                }
                
                Section(header: Text("Voice Quality")) {
                    VStack(alignment: .leading) {
                        Text("Inflection: \(settings.inflection)")
                        Slider(value: Binding(
                            get: { Float(settings.inflection) },
                            set: { settings.inflection = Int32($0); saveSettings() }
                        ), in: -300...100, step: 5)
                    }
                    VStack(alignment: .leading) {
                        Text("Head Size: \(settings.headSize)")
                        Slider(value: Binding(
                            get: { Float(settings.headSize) },
                            set: { settings.headSize = Int32($0); saveSettings() }
                        ), in: 0...6, step: 1)
                    }
                    VStack(alignment: .leading) {
                        Text("Excitation (1=breathy, 3=normal): \(settings.excitation)")
                        Slider(value: Binding(
                            get: { Float(settings.excitation) },
                            set: { settings.excitation = Int32($0); saveSettings() }
                        ), in: 1...6, step: 1)
                    }
                    VStack(alignment: .leading) {
                        Text("Unvoiced Volume (dB): \(settings.unvoiced)")
                        Slider(value: Binding(
                            get: { Float(settings.unvoiced) },
                            set: { settings.unvoiced = Int32($0); saveSettings() }
                        ), in: -70...20, step: 1)
                    }
                }
                
                Section(header: Text("System Integration")) {
                    Button("Register / Update VoiceOver Voices") {
                        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                        statusMessage = "Registered voices with System (VoiceOver)"
                    }
                }
            }
            .navigationTitle("BestSpeech Settings")
            .onAppear {
                loadEngine()
            }
        }
    }

    private func saveSettings() {
        BestSpeechSettingsStore.save(settings)
    }

    private func loadEngine() {
        // Try finding b32_tts.dll
        var resolvedURL: URL? = nil
        let possibleNames = ["b32_tts.dll", "B32_TTS.DLL"]
        for name in possibleNames {
            if let path = Bundle.main.path(forResource: name, ofType: nil) {
                resolvedURL = URL(fileURLWithPath: path)
                break
            }
            let direct = Bundle.main.bundleURL.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: direct.path) {
                resolvedURL = direct
                break
            }
        }
        if resolvedURL == nil, let groupURL = BestSpeechSettingsStore.containerURL {
            for name in possibleNames {
                let candidate = groupURL.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: candidate.path) {
                    resolvedURL = candidate
                    break
                }
            }
        }

        if let url = resolvedURL {
            synthesizer = BestSpeechSynthesizer(dllPath: url)
            statusMessage = "Engine loaded from \(url.lastPathComponent)"
        } else {
            statusMessage = "b32_tts.dll not found!"
        }
    }

    private func playTestSpeech() {
        guard let synth = synthesizer else {
            statusMessage = "Error: Engine not loaded."
            return
        }

        synth.setSpeed(settings.rate)
        synth.setPitch(settings.pitch)
        synth.setVoice(
            inflection: settings.inflection,
            headSize: settings.headSize,
            excitation: settings.excitation,
            unvoiced: settings.unvoiced
        )

        guard let samples = synth.speak(text: testText), !samples.isEmpty else {
            statusMessage = "Synthesis produced no audio."
            return
        }

        // Convert Float32 [-1, 1] to 16-bit PCM WAV data for playback
        var pcmData = Data()
        for s in samples {
            let clamped = max(-1.0, min(1.0, s))
            let int16Val = Int16(clamped * 32767.0)
            withUnsafeBytes(of: int16Val) { pcmData.append(contentsOf: $0) }
        }

        let wavData = createWavHeader(pcmData: pcmData, sampleRate: 11025, channels: 1, bitsPerSample: 16)
        
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
            try AVAudioSession.sharedInstance().setActive(true)
            audioPlayer = try AVAudioPlayer(data: wavData)
            audioPlayer?.play()
            statusMessage = "Playing audio (\(samples.count)000 samples)..."
        } catch {
            statusMessage = "Audio playback error: \(error.localizedDescription)"
        }
    }

    private func createWavHeader(pcmData: Data, sampleRate: UInt32, channels: UInt16, bitsPerSample: UInt16) -> Data {
        var header = Data()
        let blockAlign = channels * (bitsPerSample / 8)
        let byteRate = sampleRate * UInt32(blockAlign)
        let chunk2Size = UInt32(pcmData.count)
        let chunkSize = 36 + chunk2Size

        header.append(contentsOf: [0x52, 0x49, 0x46, 0x46]) // "RIFF"
        header.append(contentsOf: withUnsafeBytes(of: chunkSize.littleEndian) { Array($0) })
        header.append(contentsOf: [0x57, 0x41, 0x56, 0x45]) // "WAVE"
        header.append(contentsOf: [0x66, 0x6D, 0x74, 0x20]) // "fmt "
        header.append(contentsOf: withUnsafeBytes(of: UInt32(16).littleEndian) { Array($0) })
        header.append(contentsOf: withUnsafeBytes(of: UInt16(1).littleEndian) { Array($0) }) // PCM = 1
        header.append(contentsOf: withUnsafeBytes(of: channels.littleEndian) { Array($0) })
        header.append(contentsOf: withUnsafeBytes(of: sampleRate.littleEndian) { Array($0) })
        header.append(contentsOf: withUnsafeBytes(of: byteRate.littleEndian) { Array($0) })
        header.append(contentsOf: withUnsafeBytes(of: blockAlign.littleEndian) { Array($0) })
        header.append(contentsOf: withUnsafeBytes(of: bitsPerSample.littleEndian) { Array($0) })
        header.append(contentsOf: [0x64, 0x61, 0x74, 0x61]) // "data"
        header.append(contentsOf: withUnsafeBytes(of: chunk2Size.littleEndian) { Array($0) })
        header.append(pcmData)

        return header
    }
}
