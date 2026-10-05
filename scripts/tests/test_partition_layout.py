"""Check flash layout and the USB uploader's OTA-data address together."""
import configparser
import csv
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PartitionLayoutTests(unittest.TestCase):
    def test_nvs_and_usb_layout(self):
        config = configparser.ConfigParser()
        config.read(ROOT / "platformio.ini")
        board = config["env:Flowio-waveshare-esp32-s3"]
        path = ROOT / board["board_build.partitions"]
        partitions = {}
        end = 0x9000  # Bootloader and partition table occupy the preceding region.
        for row in csv.reader(path.read_text().splitlines()):
            if not row or row[0].lstrip().startswith("#"):
                continue
            name, kind, subtype, offset, size = [field.strip() for field in row[:5]]
            offset, size = int(offset, 0), int(size, 0)
            self.assertNotIn(name, partitions)
            self.assertGreaterEqual(offset, end, name)
            self.assertEqual(offset % (0x10000 if kind == "app" else 0x1000), 0, name)
            self.assertEqual(size % 0x1000, 0, name)
            self.assertGreater(size, 0, name)
            end = offset + size
            self.assertLessEqual(end, 16 * 1024 * 1024, name)
            partitions[name] = (offset, size, subtype)
        self.assertEqual(partitions["nvs"][1], 128 * 1024)
        self.assertEqual(partitions["otadata"][1], 0x2000)
        self.assertEqual(int(board["board_upload.arduino.boot_app0"], 0), partitions["otadata"][0])
        self.assertEqual(partitions["app0"][1], partitions["app1"][1])
        self.assertEqual(partitions["app0"][2], "ota_0")
        self.assertEqual(partitions["app1"][2], "ota_1")
        self.assertEqual(partitions["spiffs0"][1], partitions["spiffs1"][1])
        self.assertEqual(partitions["spiffs0"][2], "spiffs")
        self.assertNotEqual(partitions["spiffs1"][2], "spiffs")


if __name__ == "__main__":
    unittest.main()
