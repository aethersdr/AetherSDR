// Minimal DvkWavTransfer stub for dvk_panel_test.
//
// DvkPanel references DvkWavTransfer (upload/download + its signals), but the
// real implementation drags in RadioModel + RadioConnection + the whole radio
// command stack. The #3514 playback-visibility regression never touches WAV
// transfer, so we satisfy the linker with no-op definitions and let AUTOMOC
// generate the signal/metaobject code from the header. This keeps the test's
// link list as light as cwx_panel_test's.
#include "core/DvkWavTransfer.h"

namespace AetherSDR {

DvkWavTransfer::DvkWavTransfer(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
}

DvkWavTransfer::~DvkWavTransfer() = default;

void DvkWavTransfer::download(int, const QString&) {}
void DvkWavTransfer::upload(int, const QString&) {}
void DvkWavTransfer::cancel() {}
bool DvkWavTransfer::validateWavFile(const QString&, QString&) { return false; }

void DvkWavTransfer::onDownloadPortReceived(int, const QString&) {}
void DvkWavTransfer::onNewConnection() {}
void DvkWavTransfer::onReadyRead() {}
void DvkWavTransfer::onDownloadFinished() {}
void DvkWavTransfer::onDownloadError() {}
void DvkWavTransfer::onUploadPortReceived(int, const QString&) {}
void DvkWavTransfer::onUploadConnected() {}
void DvkWavTransfer::onUploadBytesWritten(qint64) {}
void DvkWavTransfer::onUploadError() {}
void DvkWavTransfer::sendNextChunk() {}
void DvkWavTransfer::cleanup(bool) {}
void DvkWavTransfer::finish(bool, const QString&, bool) {}

} // namespace AetherSDR
