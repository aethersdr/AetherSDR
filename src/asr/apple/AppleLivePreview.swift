// Live preview for Copy Assist: one continuous SpeechTranscriber session that
// turns 16 kHz audio into provisional text within a fraction of a second.
// The C interface is declared in AppleLivePreviewC.h; see that header for the
// contract. Built by CMake with swiftc (src/asr/apple/CMakeLists fragment in
// the top-level CMakeLists.txt), linked into aetherasr on macOS only.
//
// Fast results are on and provisional results are reported: the preview's job
// is speed. Final text in Copy Assist still comes from the selected backend.

import AVFoundation
import CoreMedia
import Foundation
import Speech

private let kRate: Double = 16000

public typealias TextCallback = @convention(c) (
    UnsafeMutableRawPointer?, UnsafePointer<CChar>?, Int64, Int64, Int32) -> Void
public typealias StateCallback = @convention(c) (UnsafeMutableRawPointer?, Int32, UnsafePointer<CChar>?) -> Void

/// Delivers callbacks until closed. The lock is held while a callback runs, so
/// close() returning means no callback is in flight and none will start.
private final class Sink: @unchecked Sendable {
    private let lock = NSLock()
    private var open = true
    private let ctx: UnsafeMutableRawPointer?
    private let onText: TextCallback
    private let onState: StateCallback

    init(ctx: UnsafeMutableRawPointer?, onText: TextCallback, onState: StateCallback) {
        self.ctx = ctx
        self.onText = onText
        self.onState = onState
    }

    func text(_ s: String, _ start: Int64, _ end: Int64, final isFinal: Bool) {
        lock.lock()
        defer { lock.unlock() }
        guard open else { return }
        s.withCString { onText(ctx, $0, start, end, isFinal ? 1 : 0) }
    }

    func state(_ code: Int32, _ message: String) {
        lock.lock()
        defer { lock.unlock() }
        guard open else { return }
        message.withCString { onState(ctx, code, $0) }
    }

    func close() {
        lock.lock()
        open = false
        lock.unlock()
    }
}

/// The language with the Mac's own region, when Apple supports that pairing:
/// Spanish on a US Mac is es-US, German on a Mac set to Germany is de-DE. nil
/// (no such pairing, or the language already names a region) leaves the choice
/// to supportedLocale(equivalentTo:), whose pick for a bare code is arbitrary
/// on macOS 26: es-CL, fr-CA, de-CH. Internal, not private, so a test can
/// compile it alongside this file.
func regionalLocale(for language: String, current: Locale, supported: [Locale]) -> Locale? {
    let wanted = Locale(identifier: language)
    guard wanted.region == nil, let code = wanted.language.languageCode,
          let region = current.region else {
        return nil
    }
    return supported.first { $0.language.languageCode == code && $0.region == region }
}

private func samples(_ t: CMTime) -> Int64 {
    Int64((t.seconds * kRate).rounded())
}

@available(macOS 26, *)
private final class Session: @unchecked Sendable {
    private let sink: Sink
    private let language: String
    private let lock = NSLock()
    private var continuation: AsyncStream<AnalyzerInput>.Continuation?
    private var analyzer: SpeechAnalyzer?
    private var analyzerFormat: AVAudioFormat?
    private var converter: AVAudioConverter?
    private var prepareTask: Task<Void, Never>?
    private var resultsTask: Task<Void, Never>?
    private var stopped = false
    private let inputFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: kRate,
                                            channels: 1, interleaved: false)!

    init(language: String, sink: Sink) {
        self.language = language
        self.sink = sink
    }

    func start() {
        prepareTask = Task { [self] in
            do {
                try await prepare()
            } catch is CancellationError {
            } catch {
                sink.state(2, "Live preview failed: \(error.localizedDescription)")
            }
        }
    }

    private func prepare() async throws {
        let wanted = language.isEmpty ? Locale.current : Locale(identifier: language)
        var chosen = regionalLocale(for: language, current: Locale.current,
                                    supported: await SpeechTranscriber.supportedLocales)
        if chosen == nil {
            chosen = await SpeechTranscriber.supportedLocale(equivalentTo: wanted)
        }
        guard let locale = chosen else {
            sink.state(2, "Live preview does not support the language \(wanted.identifier)")
            return
        }
        let transcriber = SpeechTranscriber(locale: locale, transcriptionOptions: [],
                                            reportingOptions: [.volatileResults, .fastResults],
                                            attributeOptions: [])
        // A request can come back even when the model is installed; installing
        // is then a quick no-op, so this only reports a download it cannot rule out.
        if let request = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
            let installed = await SpeechTranscriber.installedLocales.contains {
                $0.identifier(.bcp47) == locale.identifier(.bcp47)
            }
            if !installed {
                sink.state(0, "Downloading the Apple speech model for \(locale.identifier(.bcp47))…")
            }
            try await request.downloadAndInstall()
        }
        try Task.checkCancellation()
        guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [transcriber]),
              let converter = AVAudioConverter(from: inputFormat, to: format) else {
            sink.state(2, "Live preview could not agree an audio format with the speech engine")
            return
        }
        let analyzer = SpeechAnalyzer(
            modules: [transcriber],
            options: .init(priority: .userInitiated, modelRetention: .processLifetime))
        try await analyzer.prepareToAnalyze(in: format)
        try Task.checkCancellation()

        // Bounded: if the engine ever stalls, keep the newest audio rather than
        // growing without limit. ~20 s at the tap's chunk sizes.
        let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream(
            bufferingPolicy: .bufferingNewest(1000))
        let resultsTask = Task { [sink] in
            do {
                for try await r in transcriber.results {
                    sink.text(String(r.text.characters), samples(r.range.start),
                              samples(r.range.end), final: r.isFinal)
                }
            } catch {
                sink.state(2, "Live preview stopped: \(error.localizedDescription)")
            }
        }
        lock.withLock { self.resultsTask = resultsTask }
        try await analyzer.start(inputSequence: stream)

        // stop() may have run during the awaits above; it could not see this
        // analyzer, so tear it down here instead of publishing it.
        let wasStopped = lock.withLock {
            if !stopped {
                self.analyzer = analyzer
                self.analyzerFormat = format
                self.converter = converter
                self.continuation = continuation
            }
            return stopped
        }
        if wasStopped {
            continuation.finish()
            await analyzer.cancelAndFinishNow()
            return
        }
        // The locale in use, for logs and the smoke tool; the panel shows its
        // own status on Ready.
        sink.state(1, locale.identifier(.bcp47))
    }

    func feed(_ ptr: UnsafePointer<Float>, _ count: Int, _ startSample: Int64) {
        lock.lock()
        let continuation = self.continuation
        let format = self.analyzerFormat
        let converter = self.converter
        lock.unlock()
        guard let continuation, let format, let converter, count > 0,
              let input = AVAudioPCMBuffer(pcmFormat: inputFormat,
                                           frameCapacity: AVAudioFrameCount(count)) else { return }
        input.frameLength = AVAudioFrameCount(count)
        input.floatChannelData![0].update(from: ptr, count: count)

        let buffer: AVAudioPCMBuffer
        if format == inputFormat {
            buffer = input
        } else {
            let capacity = AVAudioFrameCount(Double(count) * format.sampleRate / kRate + 32)
            guard let out = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: capacity) else { return }
            var supplied = false
            var error: NSError?
            let status = converter.convert(to: out, error: &error) { _, outStatus in
                if supplied {
                    outStatus.pointee = .noDataNow
                    return nil
                }
                supplied = true
                outStatus.pointee = .haveData
                return input
            }
            guard status != .error, out.frameLength > 0 else { return }
            buffer = out
        }
        continuation.yield(AnalyzerInput(
            buffer: buffer, bufferStartTime: CMTime(value: startSample, timescale: CMTimeScale(kRate))))
    }

    func finalizeThrough(_ sample: Int64) {
        lock.lock()
        let analyzer = self.analyzer
        lock.unlock()
        guard let analyzer else { return }
        Task {
            try? await analyzer.finalize(through: CMTime(value: sample, timescale: CMTimeScale(kRate)))
        }
    }

    func stop() {
        sink.close()
        prepareTask?.cancel()
        lock.lock()
        stopped = true
        let analyzer = self.analyzer
        continuation?.finish()
        continuation = nil
        self.analyzer = nil
        let resultsTask = self.resultsTask
        lock.unlock()
        resultsTask?.cancel()
        if let analyzer {
            Task { await analyzer.cancelAndFinishNow() }
        }
    }
}

/// Opaque handle; the Session type itself is macOS 26-only.
private final class Handle {
    let session: AnyObject
    init(_ session: AnyObject) { self.session = session }
}

@_cdecl("aether_lp_is_supported")
public func aether_lp_is_supported() -> Int32 {
    if #available(macOS 26, *) {
        return SpeechTranscriber.isAvailable ? 1 : 0
    }
    return 0
}

@_cdecl("aether_lp_create")
public func aether_lp_create(_ language: UnsafePointer<CChar>?, _ ctx: UnsafeMutableRawPointer?,
                             _ onText: TextCallback?, _ onState: StateCallback?) -> UnsafeMutableRawPointer? {
    guard #available(macOS 26, *), SpeechTranscriber.isAvailable,
          let onText, let onState else { return nil }
    let session = Session(language: language.map { String(cString: $0) } ?? "",
                          sink: Sink(ctx: ctx, onText: onText, onState: onState))
    session.start()
    return Unmanaged.passRetained(Handle(session)).toOpaque()
}

@_cdecl("aether_lp_feed")
public func aether_lp_feed(_ handle: UnsafeMutableRawPointer?, _ samples: UnsafePointer<Float>?,
                           _ count: Int32, _ startSample: Int64) {
    guard #available(macOS 26, *), let handle, let samples else { return }
    let h = Unmanaged<Handle>.fromOpaque(handle).takeUnretainedValue()
    (h.session as? Session)?.feed(samples, Int(count), startSample)
}

@_cdecl("aether_lp_finalize_through")
public func aether_lp_finalize_through(_ handle: UnsafeMutableRawPointer?, _ sample: Int64) {
    guard #available(macOS 26, *), let handle else { return }
    let h = Unmanaged<Handle>.fromOpaque(handle).takeUnretainedValue()
    (h.session as? Session)?.finalizeThrough(sample)
}

@_cdecl("aether_lp_destroy")
public func aether_lp_destroy(_ handle: UnsafeMutableRawPointer?) {
    guard let handle else { return }
    let h = Unmanaged<Handle>.fromOpaque(handle).takeRetainedValue()
    if #available(macOS 26, *) {
        (h.session as? Session)?.stop()
    }
}
