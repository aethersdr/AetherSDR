import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('selection', pathlib.Path(__file__).with_name('development_selection.py'))
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)


class SelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-selection-quote-"-')
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name) / 'selection.json'
        self.item = dict(version='0.0.0-test.1', firmware='4.3.16.42034', profile=1,
                         candidate_key='11' * 32, previous_key='22' * 32,
                         files={name: dict(size=1, sha256='33' * 32) for name in selection.NAMES})

    def generate(self, item):
        self.path.write_text(json.dumps(item))
        return selection.generate(self.path)

    def test_embeds_pins_and_encodes_paths_without_reading_package(self):
        header = self.generate(self.item)
        self.assertIn('trust.candidateKey = QByteArray::fromHex("' + '11' * 32, header)
        self.assertIn(str(self.path.parent / 'package').encode().hex(), header)
        self.assertNotIn(str(self.path.parent), header)
        self.assertEqual(header.count('trust.files['), 8)
        self.assertIn('AetherPDPackage::load(', header)
        self.assertIn('package->firmware() != firmware()', header)

    def test_rejects_invalid_trust_metadata(self):
        mutations = [dict(profile=2), dict(profile=True), dict(version='x";abort();'),
                     dict(firmware='4.3.16'), dict(candidate_key='xx' * 32),
                     dict(previous_key='11'), dict(unknown=True)]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.generate(self.item | mutation)
        for size in (0, -1, 2**63, 1.5, True):
            item = copy.deepcopy(self.item)
            item['files']['LICENSES']['size'] = size
            with self.subTest(size=size), self.assertRaises(ValueError):
                self.generate(item)
        for name in selection.NAMES:
            item = copy.deepcopy(self.item)
            del item['files'][name]
            with self.subTest(missing=name), self.assertRaises(ValueError):
                self.generate(item)

    def test_rejects_combined_carrier_overflow(self):
        item = copy.deepcopy(self.item)
        for name in selection.NAMES[3:7]:
            item['files'][name]['size'] = 8 * 1024 * 1024
        with self.assertRaises(ValueError):
            self.generate(item)

    def test_rejects_duplicate_and_oversized_json(self):
        for data in ('{"version":"1.0","version":"2.0"}', ' ' * 65537):
            self.path.write_text(data)
            with self.assertRaises(ValueError):
                selection.generate(self.path)


if __name__ == '__main__':
    unittest.main()
