"""Socket-free regression cases for the disabled-reason checker."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_a11y", Path(__file__).resolve().parents[1] / "tools/check_a11y.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class DisabledReasonTest(unittest.TestCase):
    def findings(self, text):
        return checker.check_disabled_reason_is_accessible(text.splitlines())

    def test_menu_action_requires_status_tip(self):
        gate = '''void Window::gate() {
            menu->menuAction()->setEnabled(supported);
            menu->menuAction()->setToolTip(reason);
        }'''
        self.assertEqual(len(self.findings(gate)), 1)
        self.assertEqual(self.findings(gate.replace('setToolTip(reason);', 'setToolTip(reason); menu->menuAction()->setStatusTip(reason);')), [])

    def test_functions_do_not_share_receivers(self):
        self.assertEqual(self.findings('''void A::enable() { slider->setEnabled(on); }
            void A::help() { slider->setToolTip(reason); }'''), [])
        self.assertEqual(len(self.findings('''void A::gate() {
            slider->setEnabled(on); slider->setToolTip(reason);
        }
        void B::gate() { slider->setAccessibleDescription(reason); }''')), 1)

    def test_branches_do_not_suppress_each_other(self):
        self.assertEqual(len(self.findings('''void A::gate() {
            if (a) { button->setEnabled(on); button->setToolTip(reason); }
            else { button->setAccessibleDescription(reason); }
        }''')), 1)

    def test_literals_and_comments_do_not_change_scope(self):
        self.assertEqual(len(self.findings('''void A::gate() {
            const char *qss = R"style(QLabel { color: red; })style";
            // } button->setAccessibleDescription(reason);
            button->setEnabled(on);
            button->setToolTip("reason }");
        }''')), 1)

    def test_always_enabled_and_suppressed_calls(self):
        self.assertEqual(self.findings('''void A::gate() {
            button->setEnabled( true ); button->setToolTip(reason);
            other->setEnabled(false);
            other->setToolTip(reason); // a11y-check: skip-line
        }'''), [])

    def test_repeated_local_names_are_all_checked(self):
        self.assertEqual(len(self.findings('''void A::gate() {
            button->setEnabled(on); button->setToolTip(reason);
        }
        void B::gate() {
            button->setEnabled(on); button->setToolTip(reason);
        }''')), 2)


if __name__ == "__main__":
    unittest.main()
