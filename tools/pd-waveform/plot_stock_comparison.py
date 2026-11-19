#!/usr/bin/env python3
"""Export the recorded finite stock/AetherPD comparison without crowded annotations."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render(source, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    report = json.loads(source.read_text())
    assert report['production_ready'] is False, 'This exporter labels an unqualified historical comparison'
    rows = report['comparison']['aether_vs_stock_db']
    assert len(rows) == 6
    assert [(row['order'], row['upper']) for row in rows] == [
        (order, upper) for order in (3, 5, 7) for upper in (False, True)]
    for row in rows:
        stock = row['stock_dbc_per_tone']
        aether = row['aether_dbc_per_tone']
        difference = row['positive_means_aether_lower_db']
        assert all(math.isfinite(value) for value in (stock, aether, difference))
        assert abs(stock - aether - difference) < 1e-10
    output.mkdir(parents=True, exist_ok=False)
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                         'svg.hashsalt': 'aetherpd-historical-stock-comparison-v1'})
    stock_color, aether_color = '#2166ac', '#b35806'
    figure = plt.figure(figsize=(11.8, 6.8), dpi=144)
    grid = figure.add_gridspec(1, 2, left=.12, right=.96, bottom=.29, top=.77,
                              width_ratios=[1.4, 1], wspace=.12)
    chart = figure.add_subplot(grid[0, 0])
    table = figure.add_subplot(grid[0, 1], sharey=chart)
    chart.set_ylim(5.7, -1.2)
    all_values = [row[key] for row in rows for key in ['stock_dbc_per_tone', 'aether_dbc_per_tone']]
    chart.set_xlim(math.floor(min(all_values) / 5) * 5 - 5,
                   math.ceil(max(all_values) / 5) * 5 + 5)
    chart.set_yticks(range(6), [f"IMD{row['order']} {'upper' if row['upper'] else 'lower'}" for row in rows])
    chart.set_xlabel('Distortion (dBc per tone; lower is farther left)', labelpad=10)
    chart.grid(axis='x', color='#dddddd', linewidth=.7)
    chart.set_axisbelow(True)
    for spine in ['top', 'right', 'left']:
        chart.spines[spine].set_visible(False)
    chart.tick_params(axis='y', length=0, pad=10)
    table.axis('off')
    table_text = []
    positions = [.23, .58, .99]
    for x, label, color in zip(positions, ['Stock APD', 'AetherPD', 'Stock - Aether (dB)'],
                               [stock_color, aether_color, '#222222']):
        table_text.append(table.text(x, -.8, label, transform=table.get_yaxis_transform(),
                                     ha='right', va='center', fontsize=9, fontweight='bold', color=color))
    for index, row in enumerate(rows):
        stock, aether = row['stock_dbc_per_tone'], row['aether_dbc_per_tone']
        chart.plot([stock, aether], [index, index], color='#b0b0b0', linewidth=1.5, zorder=1)
        chart.scatter(stock, index, color=stock_color, marker='o', s=42, zorder=2)
        chart.scatter(aether, index, color=aether_color, marker='D', s=38, zorder=3)
        values = [f'{stock:.2f}', f'{aether:.2f}', f"{row['positive_means_aether_lower_db']:+.2f}"]
        for x, value, color in zip(positions, values, [stock_color, aether_color, '#222222']):
            table_text.append(table.text(x, index, value, transform=table.get_yaxis_transform(),
                                         ha='right', va='center', fontsize=11, color=color))
    date = report['updated_utc'][:10]
    radio = report['radio']
    assert radio['feedback_transport'] == 'local_dax_iq' and 1 <= radio['IQ_channel'] <= 4
    fdx = 'on' if radio['FDX'] else 'off'
    figure.text(.05, .95, f"{radio['model']}: recorded stock APD / AetherPD comparison", fontsize=16, fontweight='bold')
    figure.text(.05, .905, f"{date} | {radio['frequency_mhz']:.3f} MHz | nominal {radio['nominal_watts']} W | FDX {fdx} | local DAX IQ {radio['IQ_channel']}", fontsize=11)
    figure.text(.05, .86, 'Historical finite capture; final-candidate comparison remains pending.', fontsize=10)
    figure.legend(handles=[Line2D([], [], color=stock_color, marker='o', linestyle='None', label='Stock APD'),
                           Line2D([], [], color=aether_color, marker='D', linestyle='None', label='AetherPD')],
                  loc='upper right', bbox_to_anchor=(.96, .875), frameon=False, ncol=2)
    figure.text(.05, .19, 'Positive difference: AetherPD lower distortion. Negative difference: stock lower. No repeated-run uncertainty is shown.', fontsize=9)
    delta = report['comparison']['stock_on_minus_aether_on_power_db']
    figure.text(.05, .14, f'Stock corrected fundamental level was {delta:+.3f} dB higher. Input paths and calibration histories differ.', fontsize=9)
    figure.text(.05, .09, f"No calibrated RF watts or ADC headroom. Recorded runtime {report['runtime_source'][:9]}; desktop {report['desktop_source'][:9]}.", fontsize=9)
    figure.canvas.draw()
    renderer = figure.canvas.get_renderer()
    rectangles = [text.get_window_extent(renderer) for text in table_text]
    for index, bounds in enumerate(rectangles):
        assert table.get_window_extent(renderer).contains(bounds.x0, bounds.y0)
        assert table.get_window_extent(renderer).contains(bounds.x1, bounds.y1)
        assert not any(bounds.overlaps(other) for other in rectangles[index + 1:]), 'Comparison table labels overlap'
    figure.savefig(output / 'stock-comparison.png', dpi=144,
                   metadata={'Software': 'AetherSDR historical comparison plot'})
    figure.savefig(output / 'stock-comparison.svg',
                   metadata={'Date': None, 'Creator': 'AetherSDR historical comparison plot'})
    plt.close(figure)
    csv_path = output / 'stock-comparison.csv'
    with csv_path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['order', 'side', 'stock_dbc_per_tone',
            'aether_dbc_per_tone', 'stock_minus_aether_db'])
        writer.writeheader()
        for row in rows:
            writer.writerow({'order': row['order'], 'side': 'upper' if row['upper'] else 'lower',
                'stock_dbc_per_tone': row['stock_dbc_per_tone'],
                'aether_dbc_per_tone': row['aether_dbc_per_tone'],
                'stock_minus_aether_db': row['positive_means_aether_lower_db']})
    result = {'source_file': source.name, 'source_sha256': digest(source),
        'renderer_sha256': digest(Path(__file__)),
        'recorded_runtime_source': report['runtime_source'], 'recorded_desktop_source': report['desktop_source'],
        'report_date': date, 'historical_finite_comparison': True,
        'final_candidate_qualified': False, 'numeric_rows': rows,
        'table_text_count': len(table_text), 'table_labels_inside_axes_and_nonoverlapping': True,
        'matplotlib_version': matplotlib.__version__,
        'files': {name: {'bytes': (output / name).stat().st_size, 'sha256': digest(output / name)}
                  for name in ['stock-comparison.png', 'stock-comparison.svg', 'stock-comparison.csv']}}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'table_labels_nonoverlapping': True,
                      'numeric_rows': len(rows), 'final_candidate_qualified': False}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parent / 'STOCK-CANDIDATE-QUALIFICATION.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    render(args.source, args.output)
