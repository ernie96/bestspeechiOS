import SwiftUI

struct ContentView: View {
    @State private var speed: Float = 1.0
    @State private var pitch: Float = 100.0
    
    // Voice params
    @State private var inflection: Float = -150.0
    @State private var headSize: Float = 3.0
    @State private var excitation: Float = 3.0
    @State private var unvoiced: Float = -25.0
    
    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Engine Parameters")) {
                    VStack(alignment: .leading) {
                        Text("Speed multiplier: \(speed, specifier: "%.2f")")
                        Slider(value: $speed, in: 0.5...6.0)
                    }
                    VStack(alignment: .leading) {
                        Text("Pitch percentage: \(Int(pitch))%")
                        Slider(value: $pitch, in: 43...600)
                    }
                }
                
                Section(header: Text("Voice Quality")) {
                    VStack(alignment: .leading) {
                        Text("Inflection: \(Int(inflection))")
                        Slider(value: $inflection, in: -300...100)
                    }
                    VStack(alignment: .leading) {
                        Text("Head Size: \(Int(headSize))")
                        Slider(value: $headSize, in: 0...6)
                    }
                    VStack(alignment: .leading) {
                        Text("Excitation (1=breathy, 3=normal): \(Int(excitation))")
                        Slider(value: $excitation, in: 1...6)
                    }
                    VStack(alignment: .leading) {
                        Text("Unvoiced Volume (dB): \(Int(unvoiced))")
                        Slider(value: $unvoiced, in: -70...20)
                    }
                }
                
                Section(header: Text("Engine Setup")) {
                    Text("B32_TTS.DLL must be placed in the App Group shared container to work with VoiceOver.")
                        .font(.caption)
                        .foregroundColor(.gray)
                    Button("Import B32_TTS.DLL") {
                        // In a full implementation, use UIDocumentPickerViewController
                        // to let the user select the DLL and copy it to the App Group container.
                    }
                }
            }
            .navigationTitle("BestSpeech Settings")
        }
    }
}
