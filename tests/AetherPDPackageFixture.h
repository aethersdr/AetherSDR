#pragma once
#include "core/AetherPDPackage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

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
    std::optional<AetherSDR::AetherPDFirstPackage> loadFirst() const
    {
        if (!valid) { return std::nullopt; }
        AetherSDR::AetherPDFirstPackage::Files files;
        for (std::size_t i = 0; i < files.size(); ++i) { files[i] = bytes(i); }
        files[0] = decode(fixture.value("first_worker"));
        files[2] = bytes(3);
        AetherSDR::AetherPDFirstPackage::Trust firstTrust;
        firstTrust.publisherKey = trust.candidateKey;
        for (std::size_t i = 0; i < files.size(); ++i) {
            firstTrust.files[i] = {files[i].size(), QCryptographicHash::hash(files[i], QCryptographicHash::Sha256)};
        }
        return AetherSDR::AetherPDFirstPackage::verify(files, firstTrust);
    }
    std::optional<AetherSDR::AetherPDPackage> load() const
    { return valid ? AetherSDR::AetherPDPackage::load(directory.path(), trust) : std::nullopt; }
};
}
