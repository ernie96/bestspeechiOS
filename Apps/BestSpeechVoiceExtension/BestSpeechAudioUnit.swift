import Foundation
import AVFoundation
import CoreAudioKit


@objc(SpeechSynthesizerFactory)
public class SpeechSynthesizerFactory: NSObject, AUAudioUnitFactory {
    public func beginRequest(with context: NSExtensionContext) { }
    
    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        return try SpeechSynthesizer(componentDescription: componentDescription, options: [])
    }
}

public class SpeechSynthesizer: AVSpeechSynthesisProviderAudioUnit {
    private var ttsEngine: BestSpeechSynthesizer?
    private var outputBus: AUAudioUnitBus!
    private var outputBusArray: AUAudioUnitBusArray!
    
    // Audio rendering state
    private var currentBuffer: [Float] = []
    private var bufferIndex = 0
    private var isSynthesizing = false
    private let format: AVAudioFormat
    
    public override init(componentDescription: AudioComponentDescription, options: AudioComponentInstantiationOptions = []) throws {
        // Use 11025 Hz (native BestSpeech rate) to avoid complex resampling, VoiceOver will adapt.
        format = AVAudioFormat(standardFormatWithSampleRate: 11025.0, channels: 1)!
        
        try super.init(componentDescription: componentDescription, options: options)
        
        outputBus = try AUAudioUnitBus(format: format)
        outputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
        
        // Load DLL from shared container
        if let appGroupURL = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: "group.com.bestspeech") {
            let dllURL = appGroupURL.appendingPathComponent("B32_TTS.DLL")
            ttsEngine = BestSpeechSynthesizer(dllPath: dllURL)
            // Configure defaults
            ttsEngine?.setSpeed(1.0)
            ttsEngine?.setPitch(100)
            ttsEngine?.setVoice(inflection: -150, headSize: 3, excitation: 3, unvoiced: -25)
        }
    }
    
    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        return [
            AVSpeechSynthesisProviderVoice(name: "BestSpeech Default", identifier: "com.bestspeech.voice.default", primaryLanguages: ["en-US"], supportedLanguages: ["en-US"])
        ]
    }
    
    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        // Strip SSML tags simply for now, just to get text. Or just use the SSML text and remove XML.
        // A robust parser would extract <prosody rate="..."> etc.
        let text = request.ssmlRepresentation.replacingOccurrences(of: "<[^>]+>", with: "", options: .regularExpression, range: nil)
        
        // Call engine
        if let floatArray = ttsEngine?.speak(text: text) {
            self.currentBuffer = floatArray
            self.bufferIndex = 0
            self.isSynthesizing = true
        } else {
            self.isSynthesizing = false
        }
    }
    
    public override func cancelSpeechRequest() {
        self.isSynthesizing = false
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
            guard let self = self else { return noErr }
            
            let bufferList = outputData.pointee
            let buffers = UnsafeBufferPointer<AudioBuffer>(start: &bufferList.mBuffers, count: Int(bufferList.mNumberBuffers))
            
            guard let ptr = buffers[0].mData?.assumingMemoryBound(to: Float32.self) else { return noErr }
            
            var framesToCopy = Int(frameCount)
            var framesCopied = 0
            
            if self.isSynthesizing && self.bufferIndex < self.currentBuffer.count {
                let availableFrames = self.currentBuffer.count - self.bufferIndex
                let framesToWrite = min(framesToCopy, availableFrames)
                
                for i in 0..<framesToWrite {
                    ptr[i] = self.currentBuffer[self.bufferIndex + i]
                }
                
                self.bufferIndex += framesToWrite
                framesCopied = framesToWrite
                
                if self.bufferIndex >= self.currentBuffer.count {
                    self.isSynthesizing = false
                    // Notify system that synthesis is complete via actionFlags? 
                    // Action flags usually signify silence if no data
                }
            } else {
                actionFlags.pointee.insert(.unitRenderAction_OutputIsSilence)
                // Fill with silence
                for i in 0..<framesToCopy {
                    ptr[i] = 0.0
                }
            }
            
            return noErr
        }
    }
}
