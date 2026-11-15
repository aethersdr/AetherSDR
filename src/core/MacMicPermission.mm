#import <AVFoundation/AVFoundation.h>
#import <stdio.h>

void requestMicrophonePermission()
{
    // Check current authorization status first
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    switch (status) {
    case AVAuthorizationStatusNotDetermined:
        // First launch — trigger the system permission prompt
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL granted) {
            if (granted)
                fprintf(stderr, "MacMicPermission: microphone access granted\n");
            else
                fprintf(stderr, "MacMicPermission: microphone access denied by user\n");
        }];
        break;

    case AVAuthorizationStatusAuthorized:
        // Already granted — nothing to do
        break;

    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
        // Previously denied or restricted by parental controls.
        // macOS won't show the prompt again — user must go to
        // System Settings → Privacy & Security → Microphone.
        fprintf(stderr,
            "MacMicPermission: microphone access denied/restricted.\n"
            "  Open System Settings → Privacy & Security → Microphone\n"
            "  and enable access for AetherSDR.\n");
        break;
    }
}
