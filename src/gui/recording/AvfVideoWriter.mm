#import "AvfVideoWriter.h"
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include <QDebug>
#include <cstdint>
#include <cstring>

// ARC-correct ownership model for the C++/ObjC boundary:
//   - C++ stores the ObjC wrapper as void* with +1 ownership via CFBridgingRetain.
//   - Non-owning access uses __bridge.
//   - Destruction uses CFBridgingRelease (transfers back to ARC, which releases).
// No manual retain/release. Safe under default Clang ARC (no -fno-objc-arc needed).

@interface AVFWriterObjC : NSObject
@property (strong) AVAssetWriter *writer;
@property (strong) AVAssetWriterInput *videoInput;
@property (strong) AVAssetWriterInput *audioInput;
@property (strong) AVAssetWriterInputPixelBufferAdaptor *adaptor;
@property (assign) int width;
@property (assign) int height;
@property (assign) int fps;
@property (assign) BOOL initialized;
@end

@implementation AVFWriterObjC
@end

namespace {

AVFWriterObjC* asWriter(void* opaque)
{
    return (__bridge AVFWriterObjC*)opaque;
}

} // namespace

namespace AetherSDR {

AvfVideoWriter::AvfVideoWriter()
{
    // alloc/init is +1 under ARC; CFBridgingRetain keeps that +1 for C++ ownership.
    AVFWriterObjC* wrapper = [[AVFWriterObjC alloc] init];
    m_opaqueWriter = (void*)CFBridgingRetain(wrapper);
}

AvfVideoWriter::~AvfVideoWriter()
{
    close();
    if (m_opaqueWriter) {
        // Transfer ownership back to ARC; the returned id is released at end of scope.
        (void)CFBridgingRelease(m_opaqueWriter);
        m_opaqueWriter = nullptr;
    }
}

bool AvfVideoWriter::open(const QString& filePath, int width, int height, int fps)
{
    qInfo() << "[AvfVideoWriter] open() called:" << filePath
               << "width:" << width << "height:" << height << "fps:" << fps;
    AVFWriterObjC* wrapper = asWriter(m_opaqueWriter);
    if (!wrapper) {
        return false;
    }

    // Even width/height required for H.264 macroblock encoding.
    if (width < 64) {
        width = 1280;
    }
    if (height < 64) {
        height = 720;
    }
    width = (width / 2) * 2;
    height = (height / 2) * 2;

    wrapper.width = width;
    wrapper.height = height;
    wrapper.fps = fps;

    NSString* nsPath = [NSString stringWithUTF8String:filePath.toUtf8().constData()];
    NSURL* fileURL = [NSURL fileURLWithPath:nsPath];

    // Remove any pre-existing file; AVAssetWriter refuses to overwrite.
    [[NSFileManager defaultManager] removeItemAtURL:fileURL error:nil];

    NSError* error = nil;
    wrapper.writer = [AVAssetWriter assetWriterWithURL:fileURL
                                              fileType:AVFileTypeMPEG4
                                                 error:&error];
    if (error || !wrapper.writer) {
        qWarning() << "[AvfVideoWriter] Failed to create AVAssetWriter:"
                   << (error ? error.localizedDescription.UTF8String : "null writer");
        return false;
    }

    NSDictionary* videoSettings = @{
        AVVideoCodecKey: AVVideoCodecTypeH264,
        AVVideoWidthKey: @(width),
        AVVideoHeightKey: @(height),
        AVVideoCompressionPropertiesKey: @{
            AVVideoAverageBitRateKey: @(2000000) // 2 Mbps
        }
    };
    wrapper.videoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                            outputSettings:videoSettings];
    wrapper.videoInput.expectsMediaDataInRealTime = YES;

    NSDictionary* bufferAttributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey: @(width),
        (id)kCVPixelBufferHeightKey: @(height)
    };
    wrapper.adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:wrapper.videoInput
                                   sourcePixelBufferAttributes:bufferAttributes];

    if ([wrapper.writer canAddInput:wrapper.videoInput]) {
        [wrapper.writer addInput:wrapper.videoInput];
    } else {
        qWarning() << "[AvfVideoWriter] AVAssetWriter canAddInput for video failed!";
        return false;
    }

    NSDictionary* audioSettings = @{
        AVFormatIDKey: @(kAudioFormatMPEG4AAC),
        AVNumberOfChannelsKey: @(2),
        AVSampleRateKey: @(48000),
        AVEncoderBitRateKey: @(96000)
    };
    wrapper.audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                            outputSettings:audioSettings];
    wrapper.audioInput.expectsMediaDataInRealTime = YES;

    if ([wrapper.writer canAddInput:wrapper.audioInput]) {
        [wrapper.writer addInput:wrapper.audioInput];
    } else {
        qWarning() << "[AvfVideoWriter] AVAssetWriter canAddInput for audio returned false, "
                      "proceeding video-only.";
        wrapper.audioInput = nil;
    }

    if (![wrapper.writer startWriting]) {
        if (wrapper.writer.error) {
            qWarning() << "[AvfVideoWriter] AVAssetWriter startWriting failed:"
                       << wrapper.writer.error.localizedDescription.UTF8String;
        } else {
            qWarning() << "[AvfVideoWriter] AVAssetWriter startWriting failed with no error set.";
        }
        return false;
    }
    [wrapper.writer startSessionAtSourceTime:kCMTimeZero];

    wrapper.initialized = YES;
    qInfo() << "[AvfVideoWriter] open() SUCCEEDED!";
    return true;
}

void AvfVideoWriter::close()
{
    AVFWriterObjC* wrapper = asWriter(m_opaqueWriter);
    if (!wrapper || !wrapper.initialized) {
        return;
    }

    wrapper.initialized = NO;
    [wrapper.videoInput markAsFinished];
    if (wrapper.audioInput) {
        [wrapper.audioInput markAsFinished];
    }

    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    [wrapper.writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(sema);
    }];
    // Bounded wait: close() runs on the GUI thread (including app-shutdown).
    // DISPATCH_TIME_FOREVER turned any encoder stall into an unrecoverable freeze.
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sema, deadline) != 0) {
        qWarning() << "[AvfVideoWriter] finishWriting timed out after 10s; abandoning finalize.";
    }
}

bool AvfVideoWriter::writeVideoFrame(const QImage& frame, qint64 ptsUs)
{
    AVFWriterObjC* wrapper = asWriter(m_opaqueWriter);
    if (!wrapper || !wrapper.initialized) {
        return false;
    }
    // isReadyForMoreMediaData is flow control, not an error. Reporting it as
    // failure made the caller tear the whole recording down the first time the
    // H.264 encoder fell behind the 25 fps feed. PTS come from the wall clock,
    // so a dropped frame is harmless.
    if (!wrapper.videoInput.isReadyForMoreMediaData) {
        return true;
    }
    if (frame.width() < wrapper.width || frame.height() < wrapper.height) {
        return true;
    }

    CVPixelBufferRef pixelBuffer = NULL;
    CVPixelBufferPoolRef pool = wrapper.adaptor.pixelBufferPool;
    if (!pool) {
        return false;
    }
    CVReturn status = CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixelBuffer);
    if (status != kCVReturnSuccess || !pixelBuffer) {
        return false;
    }

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    void* data = CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);

    if (data) {
        for (int y = 0; y < wrapper.height; ++y) {
            std::memcpy(static_cast<uint8_t*>(data) + (static_cast<size_t>(y) * bytesPerRow),
                        frame.constScanLine(y),
                        static_cast<size_t>(wrapper.width) * 4);
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    if (!data) {
        CVPixelBufferRelease(pixelBuffer);
        return false;
    }

    CMTime pts = CMTimeMake(ptsUs, 1000000);
    BOOL success = [wrapper.adaptor appendPixelBuffer:pixelBuffer withPresentationTime:pts];
    CVPixelBufferRelease(pixelBuffer);

    return success == YES;
}

bool AvfVideoWriter::writeAudioSamples(const QByteArray& pcmData, qint64 ptsUs)
{
    AVFWriterObjC* wrapper = asWriter(m_opaqueWriter);
    if (!wrapper || !wrapper.initialized) {
        return false;
    }
    if (!wrapper.audioInput) {
        return true; // Video-only fallback session; audio samples are ignored without error
    }
    if (!wrapper.audioInput.isReadyForMoreMediaData) {
        return true;
    }

    // On-the-fly 24 kHz → 48 kHz upsample (duplicate each stereo frame).
    const int numInSamples = pcmData.size() / 4;
    if (numInSamples <= 0) {
        return true;
    }
    QByteArray upsampled(numInSamples * 2 * 4, Qt::Uninitialized);
    const int16_t* src = reinterpret_cast<const int16_t*>(pcmData.constData());
    int16_t* dst = reinterpret_cast<int16_t*>(upsampled.data());

    for (int i = 0; i < numInSamples; ++i) {
        dst[2 * i * 2]           = src[2 * i];
        dst[2 * i * 2 + 1]       = src[2 * i + 1];
        dst[(2 * i + 1) * 2]     = src[2 * i];
        dst[(2 * i + 1) * 2 + 1] = src[2 * i + 1];
    }

    // Every OSStatus is checked and every CFRelease is null-guarded:
    // CFRelease(NULL) aborts the process.
    CMBlockBufferRef blockBuffer = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        NULL, static_cast<size_t>(upsampled.size()), kCFAllocatorDefault, NULL, 0,
        static_cast<size_t>(upsampled.size()),
        kCMBlockBufferAssureMemoryNowFlag, &blockBuffer);
    if (status != noErr || !blockBuffer) {
        qWarning() << "[AvfVideoWriter] CMBlockBufferCreateWithMemoryBlock failed:" << status;
        if (blockBuffer) {
            CFRelease(blockBuffer);
        }
        return false;
    }

    char* dataPtr = nullptr;
    if (CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr, nullptr, &dataPtr) != noErr || !dataPtr) {
        qWarning() << "[AvfVideoWriter] CMBlockBufferGetDataPointer failed";
        CFRelease(blockBuffer);
        return false;
    }
    std::memcpy(dataPtr, upsampled.constData(), static_cast<size_t>(upsampled.size()));

    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = 48000;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = 4;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 4;
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel = 16;
    asbd.mReserved = 0;

    CMFormatDescriptionRef formatDesc = NULL;
    status = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, NULL, 0, NULL, NULL,
                                            &formatDesc);
    if (status != noErr || !formatDesc) {
        qWarning() << "[AvfVideoWriter] CMAudioFormatDescriptionCreate failed:" << status;
        if (formatDesc) {
            CFRelease(formatDesc);
        }
        CFRelease(blockBuffer);
        return false;
    }

    CMSampleBufferRef sampleBuffer = NULL;
    CMTime pts = CMTimeMake(ptsUs, 1000000);

    CMSampleTimingInfo timing = {
        .duration = CMTimeMake(1, 48000),
        .presentationTimeStamp = pts,
        .decodeTimeStamp = kCMTimeInvalid
    };

    CMItemCount numSamples = numInSamples * 2;
    size_t sampleSize = 4;

    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        blockBuffer,
        formatDesc,
        numSamples,
        1,
        &timing,
        1,
        &sampleSize,
        &sampleBuffer);
    if (status != noErr || !sampleBuffer) {
        qWarning() << "[AvfVideoWriter] CMSampleBufferCreateReady failed:" << status;
        if (sampleBuffer) {
            CFRelease(sampleBuffer);
        }
        CFRelease(formatDesc);
        CFRelease(blockBuffer);
        return false;
    }

    BOOL success = [wrapper.audioInput appendSampleBuffer:sampleBuffer];

    CFRelease(sampleBuffer);
    CFRelease(formatDesc);
    CFRelease(blockBuffer);

    return success == YES;
}

} // namespace AetherSDR
