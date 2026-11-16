#!/usr/bin/env python3
"""Recompute a bounded AetherPD export with NumPy, without contacting a radio.

This checks serialization, FFT scaling, averaging, peak interpolation and IMD
ratios. It cannot validate receiver headroom, absolute RF power or the PA path.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path

import numpy as np


def require(condition, message):
    if not condition:
        raise ValueError(message)


def recheck(path):
    require(path.stat().st_size <= 16 * 1024 * 1024, 'Export exceeds 16 MiB')
    data = json.loads(path.read_text())
    require(data['schema'] == 'aetherpd-linearity-v1', 'Unsupported export schema')
    require(data.get('raw_iq_format') == 'f32le-iq-v1', 'Export has no retained raw IQ')
    require(data['source'] == 'local-dax-iq' and not data['error'], 'Capture failed or uses another source')
    require(data['window'] == 'blackman-harris-7', 'This checker supports BH7 only')
    measured = data['measurement']
    require(measured['valid'], 'Export reports an invalid measurement')
    captures = data['captures']
    require(1 <= len(captures) <= 32, 'Capture count is outside the controller bound')
    require(len(captures) == measured['averages'] == data['requested_averages'], 'Average count mismatch')
    size = measured['fft_size']
    require(size == 16384, 'Unexpected snapshot size')
    rate = data['sample_rate_hz']
    require(math.isfinite(rate) and 0 < rate <= 192000, 'Invalid sample rate')
    require(len(measured['spectrum_db_raw_iq']) == size, 'Spectrum size mismatch')
    require(len({c['id'] for c in captures}) == len(captures), 'Repeated capture ID')
    stamps = [int(c['timestamp_ns']) for c in captures]
    require(stamps[0] > 0 and all(a < b for a, b in zip(stamps, stamps[1:])), 'Capture times not increasing')
    frames = []
    for capture in captures:
        require(capture['sample_count'] == size, 'Sample count mismatch')
        raw = base64.b64decode(capture['iq_f32le'], validate=True)
        require(len(raw) == size * 8, 'Raw IQ size mismatch')
        require(hashlib.sha256(raw).hexdigest() == capture['sha256'], 'Raw IQ hash mismatch')
        pairs = np.frombuffer(raw, dtype='<f4').reshape(size, 2).astype(np.float64)
        require(np.isfinite(pairs).all(), 'Nonfinite IQ')
        frames.append(pairs[:, 0] + 1j * pairs[:, 1])
    iq = np.array(frames)
    coefficients = np.array([.27105140069342, .43329793923448, .21812299954311,
                             .06592544638803, .01081174209837, .00077658482522,
                             .00001388721735])
    terms = np.arange(7)
    window = ((-1.0) ** terms * coefficients) @ np.cos(
        2 * np.pi * terms[:, None] * np.arange(size)[None, :] / size)
    transforms = np.fft.fftshift(np.fft.fft(iq * window, axis=1), axes=1) / window.sum()
    power = np.mean(np.abs(transforms) ** 2, axis=0)
    spectrum = 10 * np.log10(np.maximum(power, 1e-20))
    exported = np.array(measured['spectrum_db_raw_iq'])
    require(np.isfinite(exported).all(), 'Nonfinite exported spectrum')
    spectrum_error = float(np.max(np.abs(spectrum - exported)))
    require(spectrum_error < .001, f'Spectrum differs by {spectrum_error} dB')
    bin_hz = rate / size

    def peak(frequency):
        predicted = int(np.floor(frequency / bin_hz + .5)) + size // 2
        low, high = max(1, predicted - 7), min(size - 2, predicted + 7)
        require(low <= high, 'Peak is outside the usable band')
        index = low + int(np.argmax(power[low:high + 1]))
        # Fit independently with polynomial least squares, rather than using
        # the C++ interpolation helper or the serialized spectrum values.
        a, b, c = np.polyfit([-1., 0., 1.], spectrum[index - 1:index + 2], 2)
        offset = -b / (2 * a) if abs(a) > 5e-13 else 0.
        if abs(offset) > 1:
            offset = 0.
        return float((index - size // 2 + offset) * bin_hz), float(a * offset**2 + b * offset + c)

    tones = [peak(data[key] - data['center_hz']) for key in ('requested_tone1_hz', 'requested_tone2_hz')]
    tones.sort()
    tone_db = 10 * math.log10(sum(10 ** (p / 10) for _, p in tones) / 2)
    pep_db = 20 * math.log10(sum(10 ** (p / 20) for _, p in tones))
    sampled_db = float(10 * np.log10(np.max(np.abs(iq) ** 2)))
    require(abs(sampled_db - measured['sampled_pep_db_raw_iq']) < .001, 'Sampled PEP mismatch')
    require(abs(pep_db - measured['fundamental_pep_db_raw_iq']) < .001, 'Fundamental PEP mismatch')
    products = []
    for product in measured['products']:
        order, upper = product['order'], product['upper']
        require(order in (3, 5, 7) and isinstance(upper, bool), 'Invalid IMD product')
        spacing = tones[1][0] - tones[0][0]
        predicted = tones[1][0] + (order - 1) / 2 * spacing if upper else tones[0][0] - (order - 1) / 2 * spacing
        frequency, level = peak(predicted)
        values = {'dbc_per_tone': level - tone_db, 'dbc_fundamental_pep': level - pep_db,
                  'dbc_sampled_pep': level - sampled_db}
        require(abs(frequency - product['offset_hz']) < .001, 'IMD frequency mismatch')
        for key, value in values.items():
            require(abs(value - product[key]) < .001, f'IMD{order} {key} mismatch')
        products.append({'order': order, 'upper': upper, **values,
                         'exported_noise_limited': product['noise_limited']})
    require(len(products) == 6, 'Expected both IMD3/5/7 sidebands')
    return data, {'file': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  'frames': len(captures), 'samples_per_frame': size,
                  'max_spectrum_error_db': spectrum_error, 'products': products,
                  'fundamental_pep_db_raw_iq': pep_db,
                  'sampled_pep_db_raw_iq': sampled_db,
                  'limitations': 'Numerical cross-check only; no calibrated watts, ADC headroom or independent RF instrument.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('export', type=Path)
    parser.add_argument('--baseline', type=Path, help='Separately measured correction-OFF export')
    args = parser.parse_args()
    current, report = recheck(args.export)
    if args.baseline:
        baseline, base_report = recheck(args.baseline)
        require(current['correction_active'] is True and baseline['correction_active'] is False,
                'Comparison requires corrected current and uncorrected baseline')
        for key in ('radio_serial', 'mode', 'transmit_antenna', 'nominal_power_setting_w',
                    'center_hz', 'sample_rate_hz', 'requested_tone1_hz', 'requested_tone2_hz',
                    'requested_averages', 'window'):
            require(current[key] == baseline[key], f'Unmatched {key}')
        delta = report['fundamental_pep_db_raw_iq'] - base_report['fundamental_pep_db_raw_iq']
        require(abs(delta) <= .5, 'Fundamental power differs by more than 0.5 dB')
        report['baseline'] = base_report
        report['fundamental_power_difference_db'] = delta
        report['improvement_db'] = [
            {'order': p['order'], 'upper': p['upper'],
             'per_tone': b['dbc_per_tone'] - p['dbc_per_tone']}
            for p in report['products'] for b in base_report['products']
            if (p['order'], p['upper']) == (b['order'], b['upper'])]
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
