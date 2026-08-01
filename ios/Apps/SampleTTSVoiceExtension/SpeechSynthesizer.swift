import Foundation
import AVFoundation
import CoreAudioKit
import SampleTTSKit

@objc(SpeechSynthesizerFactory)
public class SpeechSynthesizerFactory: NSObject, AUAudioUnitFactory {
    public func beginRequest(with context: NSExtensionContext) { }
    
    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        return try SpeechSynthesizer(componentDescription: componentDescription, options: [])
    }
}

public class SpeechSynthesizer: AUAudioUnit {
    private var ttsEngine: SampleTTSEngine?
    private var outputBus: AUAudioUnitBus!
    private var outputBusArray: AUAudioUnitBusArray!
    
    public override init(componentDescription: AudioComponentDescription, options: AudioComponentInstantiationOptions = []) throws {
        try super.init(componentDescription: componentDescription, options: options)
        
        // VoiceOver usually expects 22050 Hz
        let format = AVAudioFormat(standardFormatWithSampleRate: 22050.0, channels: 1)!
        outputBus = try AUAudioUnitBus(format: format)
        outputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
        
        // Load DLL from shared container
        if let appGroupURL = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: "group.com.bestspeech") {
            let dllURL = appGroupURL.appendingPathComponent("B32_TTS.DLL")
            ttsEngine = SampleTTSEngine(dllPath: dllURL)
        }
    }
    
    public override var outputBusses: AUAudioUnitBusArray {
        return outputBusArray
    }
    
    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
    }
    
    public override func deallocateRenderResources() {
        super.deallocateRenderResources()
    }
    
    public override var internalRenderBlock: AUInternalRenderBlock {
        return { [weak self] actionFlags, timestamp, frameCount, outputBusNumber, outputData, renderEvent, pullInputBlock in
            // Audio rendering loop
            return noErr
        }
    }
}
