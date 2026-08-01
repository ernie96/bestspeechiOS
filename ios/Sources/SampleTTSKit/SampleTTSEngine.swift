import Foundation
import CSampleTTS

public class SampleTTSEngine {
    private var engine: UnsafeMutableRawPointer?

    public init?(dllPath: URL) {
        guard let data = try? Data(contentsOf: dllPath) else {
            return nil
        }
        
        engine = data.withUnsafeBytes { ptr in
            if let baseAddress = ptr.baseAddress {
                return CSampleTTS_init(baseAddress, data.count)
            }
            return nil
        }
        
        guard engine != nil else { return nil }
    }
    
    deinit {
        if let engine = engine {
            CSampleTTS_destroy(engine)
        }
    }
    
    public func setRate(_ rate: Int32) {
        CSampleTTS_set_rate(engine, rate)
    }
    
    public func setPitch(_ percent: Int32) {
        CSampleTTS_set_pitch(engine, percent)
    }
    
    public func setVoice(inflection: Int32, headSize: Int32, excitation: Int32, unvoiced: Int32) {
        CSampleTTS_set_voice(engine, inflection, headSize, excitation, unvoiced)
    }
    
    public func setSpeed(_ speed: Float) {
        CSampleTTS_set_speed(engine, speed)
    }
    
    public func speak(text: String) -> [Float]? {
        var outBytes: Int = 0
        return text.withCString { cStr -> [Float]? in
            guard let pcm = CSampleTTS_speak(engine, cStr, &outBytes) else { return nil }
            
            // Convert Int16 PCM (11025 Hz) to Float32 for VoiceOver
            let numSamples = outBytes / 2
            let floatArray = [Float](unsafeUninitializedCapacity: numSamples) { buffer, initializedCount in
                for i in 0..<numSamples {
                    // Convert Int16 to Float [-1.0, 1.0]
                    let sample = Float(pcm[i]) / 32768.0
                    buffer[i] = sample
                }
                initializedCount = numSamples
            }
            free(pcm)
            return floatArray
        }
    }
}
