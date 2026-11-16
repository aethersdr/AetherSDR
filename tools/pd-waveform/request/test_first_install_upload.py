import unittest
from run_first_install import upload_commands, removal_command

class FirstUploadTest(unittest.TestCase):
    def test_native_upload_uses_legacy_kind(self):
        self.assertEqual(upload_commands('AetherPDFirst0123456789abcdef.ssdr_waveform',9000000),
            ['file filename AetherPDFirst0123456789abcdef.ssdr_waveform','file upload 9000000 new_waveform'])
    def test_container_upload_has_distinct_kind_and_inline_filename(self):
        self.assertEqual(upload_commands('aetherpd-first-0123456789abcdef.tar.gz',2700000),
            ['file upload 2700000 waveform_docker_image aetherpd-first-0123456789abcdef.tar.gz'])
    def test_container_removal_is_not_native_uninstall(self):
        self.assertEqual(removal_command('aetherpd-first-0123456789abcdef'),'waveform remove_container aetherpd-first-0123456789abcdef')
        self.assertEqual(removal_command('AetherPDFirst0123456789abcdef'),'waveform uninstall AetherPDFirst0123456789abcdef')

if __name__=='__main__':unittest.main()
