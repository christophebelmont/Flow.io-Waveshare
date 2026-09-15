import unittest

from scripts.version_utils import normalize_config_string


class NormalizeConfigStringTests(unittest.TestCase):
    def test_plain_value_is_unchanged(self):
        self.assertEqual(normalize_config_string("2.0.1"), "2.0.1")

    def test_nested_platformio_quotes_are_removed(self):
        self.assertEqual(normalize_config_string("'\"2.0.1\"'"), "2.0.1")

    def test_surrounding_whitespace_is_removed(self):
        self.assertEqual(normalize_config_string("  '2.0.1'  "), "2.0.1")


if __name__ == "__main__":
    unittest.main()
