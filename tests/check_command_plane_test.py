"""Exercise raw-reply matching with fixtures, without changing product sources."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "command_plane", Path(__file__).resolve().parents[1] / "tools/check_command_plane.py")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class CommandPlaneTest(unittest.TestCase):
    def count(self, relative, content):
        original = CHECKER.REPO
        with tempfile.TemporaryDirectory() as directory:
            try:
                CHECKER.REPO = Path(directory)
                path = CHECKER.REPO / relative
                path.parent.mkdir(parents=True)
                path.write_text(content, encoding="utf-8")
                return CHECKER.count_for(path)
            finally:
                CHECKER.REPO = original

    def test_converted_and_new_reply_emitters(self):
        for name in ("DvkModel.cpp", "NavtexModel.cpp", "NewModel.cpp"):
            with self.subTest(name=name):
                self.assertEqual(self.count("src/models/" + name,
                    'emit replyCommandReady("text", id); Q_EMIT replyCommandReady(cmd, id);'), 2)

    def test_comments_and_literals_are_not_calls(self):
        self.assertEqual(self.count("src/models/DvkModel.cpp", '''
            // emit replyCommandReady(cmd);
            /* Q_EMIT replyCommandReady(cmd); */
            auto text = "emit replyCommandReady(cmd)";
            emit controlRequested(request, id);
        '''), 0)

    def test_cwx_ordinary_floor_is_preserved(self):
        self.assertEqual(self.count("src/models/CwxModel.cpp", '''
            emit commandReady(cmd);
            emit replyCommandReady(cmd, epoch, size);
        '''), 1)


if __name__ == "__main__":
    unittest.main()
