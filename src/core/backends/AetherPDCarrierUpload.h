#pragma once
#include "FileTransferRequest.h"
#include <QObject>
#include <QString>
#include <memory>

namespace AetherSDR {
class RadioModel;
// The backend uploads the public archive under the coordinator's reservation.
// Completion means transport completion, not installed-container publication.
class AetherPDCarrierUpload : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void begin(const QString& path, const RadioControlSession& session, const FileTransferHandle& reservation) = 0;
    virtual void cancel() = 0;
signals:
    void finished(bool success);
};
std::unique_ptr<AetherPDCarrierUpload> makeAetherPDCarrierUpload(RadioModel& model);
} // namespace AetherSDR
