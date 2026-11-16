#!/usr/bin/env python3
"""Compile reviewed local development pins; never load trust at application runtime."""
import argparse
import json
import pathlib
import re

NAMES = ("worker.envelope", "candidate.release", "previous.release", "pd-accept",
         "pd-carrier", "pd-send", "pd-deliver", "LICENSES")
FIRST_NAMES = ("worker.envelope", "candidate.release", "pd-first-request", "pd-accept",
               "pd-carrier", "pd-send", "pd-deliver", "LICENSES")
LIMITS = (184 + 512 * 1024, 18 * 1024 * 1024, 18 * 1024 * 1024,
          8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024,
          8 * 1024 * 1024, 512 * 1024)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate selection field")
        result[key] = value
    return result


def hex32(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("expected canonical 32-byte hex value")
    return value


def qstring(value):
    return 'QString::fromUtf8(QByteArray::fromHex("' + value.encode('utf-8').hex() + '"))'


def generate(selection, first=False):
    with selection.open('rb') as source:
        data = source.read(65537)
    if len(data) > 65536:
        raise ValueError("selection exceeds 64 KiB")
    item = json.loads(data, object_pairs_hook=unique_object)
    keys = {"version", "firmware", "profile", "files"} | ({"publisher_key"} if first else {"candidate_key", "previous_key"})
    if not isinstance(item, dict) or set(item) != keys:
        raise ValueError("unexpected selection fields")
    version, firmware = item['version'], item['firmware']
    if not isinstance(version, str) or len(version) > 64 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}(?:-[A-Za-z0-9.]+)?", version):
        raise ValueError("invalid development version")
    if not isinstance(firmware, str) or len(firmware) >= 28 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", firmware):
        raise ValueError("invalid firmware")
    if type(item['profile']) is not int or item['profile'] != 1:
        raise ValueError("desktop carrier currently supports profile 1 only")
    pins = item['files']
    names = FIRST_NAMES if first else NAMES
    package_type = 'AetherPDFirstPackage' if first else 'AetherPDPackage'
    namespace = 'firstDevelopment' if first else 'development'
    limits = LIMITS[:2] + (512 * 1024,) + LIMITS[3:] if first else LIMITS
    if first and hex32(item['publisher_key']) == '00' * 32:
        raise ValueError('publisher key must not be zero')
    if not isinstance(pins, dict) or set(pins) != set(names):
        raise ValueError("selection must pin exactly eight fixed package files")
    lines = ['#pragma once', '#include "core/AetherPDPackage.h"',
             'namespace AetherSDR::aetherpd::' + namespace + ' {',
             'inline QString version() { return ' + qstring(version) + '; }',
             'inline QString firmware() { return ' + qstring(firmware) + '; }',
             'inline std::optional<' + package_type + '> load() {',
             '    ' + package_type + '::Trust trust;']
    key_fields = [('publisher_key', 'publisherKey')] if first else [('candidate_key', 'candidateKey'), ('previous_key', 'previousKey')]
    for key, member in key_fields:
        lines.append('    trust.' + member + ' = QByteArray::fromHex("' + hex32(item[key]) + '");')
    for index, (name, limit) in enumerate(zip(names, limits)):
        pin = pins[name]
        if not isinstance(pin, dict) or set(pin) != {'size', 'sha256'} or type(pin['size']) is not int or not 0 < pin['size'] <= limit:
            raise ValueError("invalid bounded file pin")
        lines.append(f'    trust.files[{index}] = {{{pin["size"]}, QByteArray::fromHex("{hex32(pin["sha256"])}")}};')
    if sum(pins[name]['size'] for name in names[3:7]) > 16 * 1024 * 1024:
        raise ValueError('carrier executables exceed combined size limit')
    directory = str(selection.resolve().parent / 'package')
    lines += ['    const auto package = ' + package_type + '::load(' + qstring(directory) + ', trust);',
              '    if (!package || package->firmware() != firmware()) { return std::nullopt; }' if first else
              '    if (!package || package->profile() != 1 || package->firmware() != firmware()) { return std::nullopt; }',
              '    return package;', '}']
    if first:
        lines.append('inline QByteArray publisherKey() { return QByteArray::fromHex("' + hex32(item['publisher_key']) + '"); }')
    lines.append('}')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('selection', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--first', action='store_true')
    args = parser.parse_args()
    content = generate(args.selection, args.first)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)
