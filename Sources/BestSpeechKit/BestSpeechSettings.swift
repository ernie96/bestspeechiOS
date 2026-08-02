import Foundation

public struct BestSpeechSettings: Codable, Equatable {
    public var rate: Float
    public var pitch: Int32
    public var inflection: Int32
    public var headSize: Int32
    public var excitation: Int32
    public var unvoiced: Int32

    public init(
        rate: Float = 1.0,
        pitch: Int32 = 100,
        inflection: Int32 = -150,
        headSize: Int32 = 3,
        excitation: Int32 = 3,
        unvoiced: Int32 = -25
    ) {
        self.rate = rate
        self.pitch = pitch
        self.inflection = inflection
        self.headSize = headSize
        self.excitation = excitation
        self.unvoiced = unvoiced
    }

    public static let `default` = BestSpeechSettings()
}
