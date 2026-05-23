#!/usr/bin/env python3
"""Parse CHANGELOG.md and generate WhatsNewData.cpp with structured release data.

Usage: python3 gen_whatsnew.py <CHANGELOG.md> <output.cpp>

The generated file is compiled into the MasterSDR binary and provides
the data for the What's New dialog (#483).
"""

import re
import sys

def escape_cpp(s):
    """Escape a string for use in a C++ raw string literal."""
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')

def parse_changelog(path):
    """Parse CHANGELOG.md into a list of release dicts."""
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    releases = []
    current_release = None
    current_category = None
    current_item = None
    headline_lines = []
    in_headline = False

    for line in lines:
        line = line.rstrip('\n')

        # Release header: ## [vX.Y.Z] — YYYY-MM-DD
        m = re.match(r'^## \[v?(\d+\.\d+\.\d+(?:\.\d+)?[a-z]?)\]\s*(?:—\s*(.+))?', line)
        if m:
            # Save previous item
            if current_item and current_release:
                current_release['items'].append(current_item)
                current_item = None

            current_release = {
                'version': m.group(1),
                'date': (m.group(2) or '').strip(),
                'headline': '',
                'items': []
            }
            releases.append(current_release)
            current_category = None
            in_headline = False
            continue

        if not current_release:
            continue

        # Section header: ### Category or ### Headline
        if line.startswith('### '):
            # Save previous item
            if current_item:
                current_release['items'].append(current_item)
                current_item = None

            section = line[4:].strip()

            # Categorize the section
            sl = section.lower()
            if 'new feature' in sl or 'feature' in sl:
                current_category = 'Feature'
                in_headline = False
            elif 'bug fix' in sl and 'improvement' in sl:
                current_category = 'BugFix'  # mixed section, default to BugFix
                in_headline = False
            elif 'bug fix' in sl or 'fix' in sl:
                current_category = 'BugFix'
                in_headline = False
            elif 'improvement' in sl:
                current_category = 'Improvement'
                in_headline = False
            elif 'infrastructure' in sl:
                current_category = 'Infrastructure'
                in_headline = False
            else:
                # Treat as headline (e.g., "### Digital-Friendly Minimal Mode")
                if not current_release['headline']:
                    current_release['headline'] = section
                current_category = None
                in_headline = True
            continue

        # Bold item title: **Title (#NNN)** or **Title**
        m = re.match(r'^\*\*(.+?)\*\*', line)
        if m and current_category:
            # Save previous item
            if current_item:
                current_release['items'].append(current_item)

            title = m.group(1).strip()
            # Remove trailing issue references for cleaner display
            title = re.sub(r'\s*\(#\d+(?:,\s*#\d+)*\)\s*$', '', title)
            current_item = {
                'category': current_category,
                'title': title,
                'desc_lines': []
            }
            continue

        # Bullet line under a bold item
        if line.startswith('- ') and current_item:
            current_item['desc_lines'].append(line[2:].strip())
            continue

        # Continuation of a bullet (indented)
        if line.startswith('  ') and current_item and current_item['desc_lines']:
            current_item['desc_lines'][-1] += ' ' + line.strip()
            continue

    # Save last item
    if current_item and current_release:
        current_release['items'].append(current_item)

    return releases


def generate_cpp(releases, output_path):
    """Generate WhatsNewData.cpp from parsed releases."""
    cat_map = {
        'Feature': 'ChangeCategory::Feature',
        'BugFix': 'ChangeCategory::BugFix',
        'Improvement': 'ChangeCategory::Improvement',
        'Infrastructure': 'ChangeCategory::Infrastructure',
    }

    lines = []
    lines.append('// Auto-generated from CHANGELOG.md by scripts/gen_whatsnew.py')
    lines.append('// Do not edit — changes will be overwritten on next build.')
    lines.append('#include "generated/WhatsNewData.h"')
    lines.append('')
    lines.append('namespace MasterSDR {')
    lines.append('')
    lines.append('const std::vector<ReleaseEntry>& whatsNewEntries() {')
    lines.append('    static const std::vector<ReleaseEntry> entries = {')

    for rel in releases:
        if not rel['items']:
            continue
        lines.append('        {QStringLiteral("%s"), QStringLiteral("%s"), QStringLiteral("%s"), {' % (
            escape_cpp(rel['version']),
            escape_cpp(rel['date']),
            escape_cpp(rel['headline']),
        ))
        for item in rel['items']:
            desc = ' '.join(item['desc_lines'])
            if len(desc) > 200:
                desc = desc[:197] + '...'
            lines.append('            {%s, QStringLiteral("%s"), QStringLiteral("%s")},' % (
                cat_map.get(item['category'], 'ChangeCategory::Feature'),
                escape_cpp(item['title']),
                escape_cpp(desc),
            ))
        lines.append('        }},')

    lines.append('    };')
    lines.append('    return entries;')
    lines.append('}')
    lines.append('')
    lines.append('} // namespace MasterSDR')
    lines.append('')

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <CHANGELOG.md> <output.cpp>', file=sys.stderr)
        sys.exit(1)

    releases = parse_changelog(sys.argv[1])
    generate_cpp(releases, sys.argv[2])
    print(f'Generated {sys.argv[2]} with {len(releases)} releases, '
          f'{sum(len(r["items"]) for r in releases)} items')
