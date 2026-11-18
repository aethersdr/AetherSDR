#pragma once
#include "core/AetherPDPackage.h"
#include "../third_party/monocypher/monocypher-ed25519.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cstdint>

namespace AetherPDTest {
// Test-only public pins and signing keys. Production must not read its trust
// from the directory containing the package it is checking.
struct PackageFixture {
    QTemporaryDir directory;
    AetherSDR::AetherPDPackage::Trust trust;
    QJsonObject fixture;
    bool valid{false};
    explicit PackageFixture(int arch = 64)
    {
        QFile file(QStringLiteral(AETHERPD_PACKAGE_TESTDATA) + QStringLiteral("/package-%1.json").arg(arch));
        if (!directory.isValid() || !file.open(QIODevice::ReadOnly)) { return; }
        fixture = QJsonDocument::fromJson(file.readAll()).object();
        trust.candidateKey = decode(fixture.value("candidate_key"));
        trust.previousKey = decode(fixture.value("previous_key"));
        const auto names = AetherSDR::AetherPDPackage::fileNames();
        for (std::size_t i = 0; i < names.size(); ++i) {
            const QJsonObject pin = fixture.value("pins").toObject().value(names[i]).toObject();
            trust.files[i] = {pin.value("size").toInteger(), decode(pin.value("sha256"))};
            if (!write(i, decode(fixture.value("files").toObject().value(names[i])))) { return; }
        }
        valid = true;
    }
    static QByteArray decode(const QJsonValue& value) { return QByteArray::fromHex(value.toString().toLatin1()); }
    QString path(std::size_t index) const
    { return QDir(directory.path()).filePath(AetherSDR::AetherPDPackage::fileNames()[index]); }
    QByteArray bytes(std::size_t index) const
    {
        QFile file(path(index));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
    bool write(std::size_t index, const QByteArray& bytes, bool repin = false)
    {
        QFile file(path(index));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) { return false; }
        if (repin) { trust.files[index] = {bytes.size(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)}; }
        return true;
    }
    QByteArray primaryUnit() const
    {
        const QByteArray release = bytes(1);
        if (release.size() < 432) { return {}; }
        qsizetype offset = 432;
        for (int i = 0; i < 7; ++i) { offset += qFromBigEndian<quint32>(release.constData() + 80 + i * 36); }
        return release.mid(offset);
    }
    bool replacePrimaryUnit(const QByteArray& unit)
    {
        // Re-sign public test data and both worker bindings, so a refusal tests
        // service semantics rather than a stale signature, pin or release hash.
        QByteArray seed = QCryptographicHash::hash("public desktop package fixture candidate", QCryptographicHash::Sha256);
        std::array<uint8_t, 64> secret{};
        std::array<uint8_t, 32> publicKey{};
        crypto_ed25519_key_pair(secret.data(), publicKey.data(), reinterpret_cast<uint8_t*>(seed.data()));
        if (QByteArray(reinterpret_cast<const char*>(publicKey.data()), publicKey.size()) != trust.candidateKey) { return false; }
        const auto sign = [&](const QByteArray& data) {
            QByteArray signature(64, '\0');
            crypto_ed25519_sign(reinterpret_cast<uint8_t*>(signature.data()), secret.data(),
                reinterpret_cast<const uint8_t*>(data.constData()), data.size());
            return signature;
        };
        const auto blake = [](const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Blake2b_256); };
        const QByteArray original = bytes(1);
        if (original.size() < 432 || unit.isEmpty() || unit.size() > 16384) { return false; }
        const quint32 oldSize = qFromBigEndian<quint32>(original.constData() + 332);
        QByteArray manifest = original.first(368);
        qToBigEndian<quint32>(unit.size(), manifest.data() + 332);
        manifest.replace(336, 32, blake(unit));
        const QByteArray candidate = manifest + sign(manifest) + original.mid(432, original.size() - 432 - oldSize) + unit;
        const auto rebind = [&](const QByteArray& originalWorker) {
            if (originalWorker.size() <= 184) { return QByteArray{}; }
            QByteArray header = originalWorker.first(120);
            header.replace(56, 32, blake(candidate));
            return header + sign(header) + originalWorker.mid(184);
        };
        const QByteArray worker = rebind(bytes(0));
        if (worker.isEmpty() || !write(1, candidate, true) || !write(0, worker, true)) { return false; }
        if (fixture.contains("first_worker")) {
            fixture.insert("first_worker", QString::fromLatin1(rebind(decode(fixture.value("first_worker"))).toHex()));
        }
        return true;
    }
    bool enableControlRecovery()
    {
        QByteArray unit = primaryUnit();
        unit.replace("StandardOutput=journal\n", "Restart=on-failure\nRestartSec=5\nStartLimitInterval=60\n"
            "StartLimitBurst=3\nStartLimitAction=none\nStandardOutput=journal\n");
        return replacePrimaryUnit(unit);
    }
    AetherSDR::AetherPDFirstPackage::Files firstFiles() const
    {
        AetherSDR::AetherPDFirstPackage::Files files;
        for (std::size_t i = 0; i < files.size(); ++i) { files[i] = bytes(i); }
        files[0] = decode(fixture.value("first_worker"));
        files[2] = bytes(3);
        return files;
    }
    AetherSDR::AetherPDFirstPackage::Trust firstTrust() const
    {
        const auto files = firstFiles();
        AetherSDR::AetherPDFirstPackage::Trust firstTrust;
        firstTrust.publisherKey = trust.candidateKey;
        for (std::size_t i = 0; i < files.size(); ++i) {
            firstTrust.files[i] = {files[i].size(), QCryptographicHash::hash(files[i], QCryptographicHash::Sha256)};
        }
        return firstTrust;
    }
    std::optional<AetherSDR::AetherPDFirstPackage> loadFirst() const
    {
        return valid ? AetherSDR::AetherPDFirstPackage::verify(firstFiles(), firstTrust()) : std::nullopt;
    }
    std::optional<AetherSDR::AetherPDPackage> load() const
    { return valid ? AetherSDR::AetherPDPackage::load(directory.path(), trust) : std::nullopt; }
};
}
