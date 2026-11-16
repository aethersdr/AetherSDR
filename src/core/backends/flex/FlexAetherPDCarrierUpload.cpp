#include "core/backends/AetherPDCarrierUpload.h"
#include "core/WaveformInstaller.h"

namespace AetherSDR {
namespace {
class FlexCarrierUpload final : public AetherPDCarrierUpload {
public:
    explicit FlexCarrierUpload(RadioModel& model) : m_upload(&model)
    {
        connect(&m_upload, &WaveformInstaller::finished, this,
            [this](bool success, const QString&) { emit finished(success); });
    }
    void begin(const QString& path, const RadioControlSession& session, const FileTransferHandle& reservation) override
    { m_upload.installReservedDockerWaveform(path, session, reservation); }
    void beginNative(const QString& path, const RadioControlSession& session, const FileTransferHandle& reservation) override
    { m_upload.installReservedLegacyWaveform(path, session, reservation); }
    void cancel() override { m_upload.cancel(); }
private:
    WaveformInstaller m_upload;
};
}
std::unique_ptr<AetherPDCarrierUpload> makeAetherPDCarrierUpload(RadioModel& model)
{
    // WaveformInstaller dispatches typed requests through RadioModel's original
    // session/capability fence. Unsupported backends cannot admit this upload.
    return std::make_unique<FlexCarrierUpload>(model);
}
} // namespace AetherSDR
