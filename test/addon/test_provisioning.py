"""A new board set up at installation: the NVS image (nvs_image.py), the
factory image with it, and linking the board with the key it was given
(server.py, "a new board, set up at installation").
Run: python3 test/addon/test_provisioning.py"""

import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADDON = ROOT / "ha-addon" / "matter_builder"
sys.path.insert(0, str(ADDON))
os.environ.setdefault("MB_DATA", tempfile.mkdtemp(prefix="mb-test-"))
os.environ.setdefault("MB_SRC", str(ROOT))
if "server" in sys.modules:
    server = sys.modules["server"]
else:
    spec = importlib.util.spec_from_file_location("server", ADDON / "server.py")
    server = importlib.util.module_from_spec(spec)
    sys.modules["server"] = server
    spec.loader.exec_module(server)
import nvs_image  # noqa: E402

DATASET = bytes(range(1, 101))
TOKEN = "0" * 48
SCRIPT = "\n".join(f"io set {i % 8} relay {i % 20} Connection {i}" for i in range(300))
# sha256 of what ESP-IDF 5.5's own generator makes of the same values
# (python -m esp_idf_nvs_partition_gen generate <csv> out.bin 0x10000)
IDF_REFERENCE = "21ed2de40ab580ad7fcf7c527fd572cf84bd74f2146061ce68425cd18428b23d"
IDF_PYTHON = Path.home() / ".espressif/python_env/idf5.5_py3.14_env/bin/python"
IDF_NVS_TOOL = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5.5")) / \
    "components/nvs_flash/nvs_partition_tool/nvs_tool.py"


def reference_image():
    nvs = nvs_image.Image()
    nvs.blob("thread", "dataset", DATASET)
    nvs.string("cfgmode", "ssid", "Thuis netwerk")
    nvs.string("cfgmode", "pass", "geheim wachtwoord")
    nvs.blob("access", "clients", b"Home Assistant".ljust(24, b"\0") + hashlib.sha256(TOKEN.encode()).digest())
    nvs.blob("mbprov", "script", SCRIPT.encode())
    return nvs.build(0x10000)


def factory_image(nvs_offset=0x9000, nvs_size=0x10000):
    """A factory image as merge_bin makes it: bootloader, partition table at
    0x8000, an empty NVS partition, the app at 0x20000."""
    image = bytearray(b"\xff") * 0x30000
    image[0:4] = b"BOOT"
    rows = [("nvs", 1, 2, nvs_offset, nvs_size), ("otadata", 1, 0, 0x19000, 0x2000), ("app0", 0, 0x10, 0x20000, 0x300000)]
    for i, (name, kind, sub, offset, size) in enumerate(rows):
        row = b"\xaa\x50" + struct.pack("<BBII", kind, sub, offset, size) + name.encode().ljust(16, b"\0") + b"\0" * 4
        image[0x8000 + 32 * i:0x8000 + 32 * (i + 1)] = row
    image[0x20000:0x20004] = b"APP!"
    return bytes(image)


class NvsImage(unittest.TestCase):
    def test_same_bytes_as_esp_idf(self):
        self.assertEqual(hashlib.sha256(reference_image()).hexdigest(), IDF_REFERENCE)

    @unittest.skipUnless(IDF_PYTHON.exists() and IDF_NVS_TOOL.exists(), "ESP-IDF not installed")
    def test_esp_idf_reads_it(self):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(reference_image())
        try:
            out = subprocess.run([str(IDF_PYTHON), str(IDF_NVS_TOOL), "-i", f.name, "-d", "minimal", "--integrity-check"],
                                 capture_output=True, text=True).stdout
        finally:
            os.unlink(f.name)
        self.assertIn("cfgmode:ssid = Thuis netwerk", out)
        self.assertIn("mbprov:script[2]", out)  # a blob over three pages
        self.assertNotIn("Error", out)

    def test_limits(self):
        nvs = nvs_image.Image()
        with self.assertRaises(ValueError):
            nvs.string("ns", "s", "x" * nvs_image.MAX_STRING)
        with self.assertRaises(ValueError):
            nvs_image.header(1, nvs_image.U8, 1, 0xFF, "a_key_that_is_too_long")
        nvs.blob("ns", "big", b"x" * 0x10000)
        with self.assertRaises(ValueError):
            nvs.build(0x10000)


class FactoryImage(unittest.TestCase):
    def test_nvs_goes_into_the_empty_partition(self):
        image = factory_image()
        self.assertEqual(nvs_image.nvs_partition(image), (0x9000, 0x10000))
        out = nvs_image.with_nvs(image, reference_image())
        self.assertEqual(out[0x9000:0x19000], reference_image())
        self.assertEqual(out[0:0x9000], image[0:0x9000])
        self.assertEqual(out[0x19000:], image[0x19000:])

    def test_never_over_data(self):
        image = bytearray(factory_image())
        image[0x9100] = 0
        with self.assertRaises(ValueError):
            nvs_image.with_nvs(bytes(image), reference_image())
        with self.assertRaises(ValueError):
            nvs_image.with_nvs(b"\xff" * 0x10000, reference_image())  # no partition table


class Provisioning(unittest.TestCase):
    def setUp(self):
        for f in (server.PRELINKED, server.PENDING_RECIPES, server.PROXIES):
            f.unlink(missing_ok=True)
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (server.ha_thread_dataset, server.load_wifi_default)
        server.ha_thread_dataset = lambda: DATASET
        server.load_wifi_default = lambda: {"ssid": "Thuis", "password": "geheim"}

    def tearDown(self):
        server.ha_thread_dataset, server.load_wifi_default = self.saved

    def image(self, app="mb_generic"):
        path = self.dir / f"{app}-0.1.35-factory.bin"
        path.write_bytes(factory_image())
        return path

    def test_thread_board_gets_network_key_and_recipe(self):
        recipe = {"text": "device:\n  name: Qlima\n", "name": "Qlima", "lines": ["io set 0 relay 4 Power", "name Qlima"]}
        out, summary = server.provisioned_image(self.image(), "ac:eb:e6:c9:4f:8c", wifi=True, recipe=recipe)
        self.assertEqual(summary, {"provisioned": True, "board": "ACEBE6C94F8C", "thread": True,
                                   "wifi": "Thuis", "recipe": True})
        nvs = out[0x9000:0x19000]
        self.assertIn(DATASET, nvs)
        self.assertIn(b"io set 0 relay 4 Power\nname Qlima", nvs)
        self.assertIn(b"Thuis\0", nvs)
        token = server.load_prelinked()["ACEBE6C94F8C"]["token"]
        self.assertEqual(len(token), 48)
        self.assertIn(b"Home Assistant".ljust(24, b"\0") + hashlib.sha256(token.encode()).digest(), nvs)

    def test_wifi_only_when_chosen_and_no_thread_for_wifi_firmware(self):
        out, summary = server.provisioned_image(self.image("mb_generic_wifi"), "ac:eb:e6:c9:4f:8c")
        self.assertFalse(summary["thread"])
        self.assertIsNone(summary["wifi"])
        self.assertNotIn(DATASET, out)
        self.assertNotIn(b"Thuis\0", out)

    def test_recipe_script_only_for_generic_firmware(self):
        recipe = {"lines": ["name Bed"]}
        _, summary = server.provisioned_image(self.image("octo_bridge"), "ac:eb:e6:c9:4f:8c", recipe=recipe)
        self.assertFalse(summary["recipe"])

    def test_without_mac_the_plain_image(self):
        image = self.image()
        out, summary = server.provisioned_image(image, "")
        self.assertEqual((out, summary), (image.read_bytes(), {"provisioned": False}))

    def test_only_recipe_lines(self):
        for bad in ["wifi ssid x", "thread dataset 00", "access revoke 0", "name a\nwifi exit"]:
            with self.assertRaises(ValueError):
                server.provision_script([bad])
        self.assertEqual(server.provision_script([" io set 1 led 5 Lamp ", "", "rule set 0 when=boot"]),
                         "io set 1 led 5 Lamp\nrule set 0 when=boot")


class LinkedWhenItShowsUp(unittest.TestCase):
    RECIPE = "device:\n  name: Qlima D620\n  app: generic\n"

    def setUp(self):
        for f in (server.PRELINKED, server.PENDING_RECIPES, server.PROXIES):
            f.unlink(missing_ok=True)
        self.saved = (server.request, server.proxy_name, server.dismiss)
        self.replies = {}
        server.request = lambda host, path, token, body=None, **k: self.replies.get((token, body), (401, "wrong key"))
        server.proxy_name = lambda host, token: "Qlima D620"
        server.dismiss = lambda device_id: None
        self.device = {"id": "ACEBE6C94F8C", "app": "mb_generic", "host": "76EE82AFBAB2D1B9.local", "address": "fd7e::1"}

    def tearDown(self):
        server.request, server.proxy_name, server.dismiss = self.saved

    def prelink(self, in_firmware=True):
        server.save_prelinked({"ACEBE6C94F8C": {"token": "k" * 48, "added": 0, "recipe_in_firmware": in_firmware}})
        server.pending_recipe_add("ACEBE6C94F8C", self.RECIPE, "Qlima D620")
        self.replies[("k" * 48, b"json")] = (200, '{"io":{"name":"Qlima D620"}}')

    def test_recipe_ran_on_the_device(self):
        self.prelink()
        self.replies[("k" * 48, b"provision")] = (200, "ok, 12 lines\n")
        proxy = server.link_prelinked(self.device)
        self.assertEqual((proxy["token"], proxy["device_id"], proxy["name"]), ("k" * 48, "ACEBE6C94F8C", "Qlima D620"))
        self.assertNotIn("recipe", server.load_proxies()[0])
        self.assertEqual(server.load_prelinked(), {})
        self.assertIsNone(server.link_prelinked(self.device))  # once

    def test_older_firmware_gets_the_recipe_from_the_page(self):
        self.prelink()
        self.replies[("k" * 48, b"provision")] = (200, "unknown command: provision\n")
        server.link_prelinked(self.device)
        self.assertEqual(server.load_proxies()[0]["recipe"]["name"], "Qlima D620")

    def test_not_its_key_then_nothing(self):
        self.prelink()
        self.replies.clear()  # flashed again elsewhere: the key is gone
        self.assertIsNone(server.link_prelinked(self.device))
        self.assertEqual(server.load_proxies(), [])
        self.assertIn("ACEBE6C94F8C", server.load_prelinked())

    def test_unknown_board(self):
        self.assertIsNone(server.link_prelinked(self.device))


class ThreadNetworkGiven(unittest.TestCase):
    """A linked Thread device in configuration mode without a network gets HA's."""

    def setUp(self):
        self.sent = []
        self.saved = (server.request, server.ha_thread_dataset)
        self.source = "none"

        def request(host, path, token, body=None, **k):
            self.sent.append(body)
            if body == b"thread json":
                return 200, json.dumps({"source": self.source, "name": "", "channel": 0, "pan_id": "0x0000"})
            return 200, "ok, joined at the next start\n" if body.startswith(b"thread dataset ") else "ok\n"
        server.request = request
        server.ha_thread_dataset = lambda: DATASET
        self.proxy = {"name": "Qlima", "token": "k" * 48}
        self.device = {"id": "ACEBE6C94F8C", "network": "thread", "address": "192.168.1.50"}

    def tearDown(self):
        server.request, server.ha_thread_dataset = self.saved

    def test_given_and_back_to_normal(self):
        self.assertTrue(server.give_thread_network(self.proxy, self.device))
        self.assertEqual(self.sent, [b"thread json", b"thread dataset " + DATASET.hex().encode(), b"wifi exit"])

    def test_not_when_it_has_one_or_is_on_wifi(self):
        self.source = "page"
        self.assertFalse(server.give_thread_network(self.proxy, self.device))
        self.assertFalse(server.give_thread_network(self.proxy, {**self.device, "network": "wifi"}))
        self.assertEqual(self.sent, [b"thread json"])


if __name__ == "__main__":
    unittest.main()
