"""Tests for unused-function linting and compound-literal calls."""

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "check_unused_functions", ROOT / "scripts/check-unused-functions.py",
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def report(name="helper", kind="unusedFunction"):
    root = ET.Element("results")
    errors = ET.SubElement(root, "errors")
    error = ET.SubElement(errors, "error", {
        "id": kind, "severity": "style",
        "msg": f"The function '{name}' is never used.",
    })
    ET.SubElement(error, "location", {"file": "probe.c", "line": "1"})
    return ET.tostring(root, encoding="unicode")


class UnusedFunctionTests(unittest.TestCase):
    def test_summary_call(self):
        calls = CHECK.summary_calls("caller call:[helper]\n")
        self.assertIn("helper", calls)
        self.assertEqual([], CHECK.remaining_errors(report(), calls))

    def test_multiple_calls(self):
        self.assertEqual({"first", "second"}, CHECK.summary_calls(
            "caller global:[state] call:[first,second] noreturn:[second]\n",
        ))

    def test_empty_summary(self):
        self.assertEqual(set(), CHECK.summary_calls("caller\n"))

    def test_removed_call_fails(self):
        self.assertEqual(1, len(CHECK.remaining_errors(report(), set())))

    def test_recursive_call_fails(self):
        calls = CHECK.summary_calls("helper call:[helper] noreturn:[helper]\n")
        self.assertNotIn("helper", calls)
        self.assertEqual(1, len(CHECK.remaining_errors(report(), calls)))

    def test_other_diagnostics_fail(self):
        self.assertEqual(1, len(CHECK.remaining_errors(
            report(kind="syntaxError"), {"helper"},
        )))

    def test_missing_location_fails(self):
        xml = '<results><errors><error id="unusedFunction" msg="bad"/></errors></results>'
        self.assertEqual(1, len(CHECK.remaining_errors(xml, set())))

    def test_malformed_report_fails(self):
        with self.assertRaises(ET.ParseError):
            CHECK.remaining_errors("not XML", set())

    @unittest.skipUnless(shutil.which("cppcheck"), "cppcheck not installed")
    def test_cppcheck_integration(self):
        source = '''
typedef struct { int x; } Str;
static void used(Str s) { (void)s; }
static void recursive(Str s) { if (s.x) recursive((Str){s.x - 1}); }
void unused(void) {}
void disabled(Str s) { (void)s; }
int main(void) {
    used((Str){0});
    const char *text = "disabled((Str){0});";
    (void)text;
    /* disabled((Str){0}); */
#if 0
    disabled((Str){0});
#endif
    return 0;
}
'''
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            path = Path(directory) / "probe.c"
            path.write_text(source)
            result = subprocess.run([
                sys.executable, str(ROOT / "scripts/check-unused-functions.py"), str(path),
            ], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(1, result.returncode, result.stderr)
        for name in ("unused", "recursive", "disabled"):
            self.assertIn(f"The function '{name}' is never used.", result.stderr)
        self.assertNotIn("The function 'used'", result.stderr)


if __name__ == "__main__":
    unittest.main()
