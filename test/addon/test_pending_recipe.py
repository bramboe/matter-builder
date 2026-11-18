"""A recipe installed on a new board waits in the add-on under the board's
MAC and is handed over when that board is linked (server.py, "a recipe on a
new board"). Run: python3 test/addon/test_pending_recipe.py"""

import importlib.util
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = tempfile.mkdtemp(prefix="mb-test-")
os.environ["MB_DATA"] = DATA
os.environ.setdefault("MB_SRC", str(ROOT))
spec = importlib.util.spec_from_file_location("server", ROOT / "ha-addon" / "matter_builder" / "server.py")
server = importlib.util.module_from_spec(spec)
sys.modules["server"] = server
spec.loader.exec_module(server)

RECIPE = "device:\n  name: Qlima D620\n  app: generic\n\nconnections:\n  - slot: 0\n    type: relay\n    pin: 4\n    name: Power\n"

# esptool's flash_id on an ESP32-C6 (v4.x): the base MAC, then the 8-byte EUI-64
ESPTOOL_C6 = """Chip is ESP32-C6 (QFN40) (revision v0.1)
Features: WiFi 6, BT 5, IEEE802.15.4
Crystal is 40MHz
BASE MAC: ac:eb:e6:c9:4f:8c
MAC_EXT: ff:fe
MAC: ac:eb:e6:ff:fe:c9:4f:8c
Detected flash size: 8MB
"""
# The classic ESP32 prints only "MAC:"
ESPTOOL_ESP32 = """Chip is ESP32-D0WD-V3 (revision v3.1)
MAC: 24:0a:c4:12:34:56
Detected flash size: 4MB
"""


class PendingRecipe(unittest.TestCase):
    def setUp(self):
        server.PENDING_RECIPES.unlink(missing_ok=True)

    def test_board_mac_prefers_base_mac(self):
        self.assertEqual(server.board_mac(ESPTOOL_C6), "ac:eb:e6:c9:4f:8c")
        self.assertEqual(server.board_mac(ESPTOOL_ESP32), "24:0a:c4:12:34:56")
        self.assertEqual(server.board_mac("no esp here"), "")

    def test_board_id_is_the_firmware_id(self):
        # /info gives the base MAC as 12 upper-case hex digits
        self.assertEqual(server.board_id("ac:eb:e6:c9:4f:8c"), "ACEBE6C94F8C")
        self.assertEqual(server.board_id("ACEBE6C94F8C"), "ACEBE6C94F8C")
        self.assertEqual(server.board_id("ac:eb:e6:ff:fe:c9:4f:8c"), "")  # an EUI-64 is not a board id
        self.assertEqual(server.board_id(None), "")

    def test_taken_once_by_the_board_it_was_installed_on(self):
        key = server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Qlima D620", "qlima-d620")
        self.assertEqual(key, "ACEBE6C94F8C")
        self.assertIsNone(server.pending_recipe_take("111111111111", "mb_generic"))
        entry = server.pending_recipe_take("ACEBE6C94F8C", "mb_generic")
        self.assertEqual((entry["name"], entry["slug"], entry["text"]), ("Qlima D620", "qlima-d620", RECIPE))
        self.assertIsNone(server.pending_recipe_take("ACEBE6C94F8C", "mb_generic"))

    def test_survives_a_restart_of_the_add_on(self):
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Qlima D620")
        on_disk = json.loads(server.PENDING_RECIPES.read_text())
        self.assertIn("ACEBE6C94F8C", on_disk)

    def test_newest_install_on_a_board_wins(self):
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "First")
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Second")
        self.assertEqual(server.pending_recipe_take("ACEBE6C94F8C", "mb_generic")["name"], "Second")

    def test_unknown_board_goes_to_the_next_generic_device(self):
        self.assertEqual(server.pending_recipe_add("", RECIPE, "Downloaded"), server.UNKNOWN_BOARD)
        self.assertIsNone(server.pending_recipe_take("222222222222", "octo_bridge"))
        self.assertEqual(server.pending_recipe_take("222222222222", "mb_generic_wifi")["name"], "Downloaded")
        self.assertIsNone(server.pending_recipe_take("333333333333", "mb_generic"))

    def test_mac_match_wins_over_unknown_board(self):
        server.pending_recipe_add("", RECIPE, "Downloaded")
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Installed here")
        self.assertEqual(server.pending_recipe_take("ACEBE6C94F8C", "mb_generic")["name"], "Installed here")
        self.assertEqual(server.pending_recipe_take("444444444444", "mb_generic")["name"], "Downloaded")

    def test_old_and_empty_recipes(self):
        self.assertIsNone(server.pending_recipe_add("ac:eb:e6:c9:4f:8c", "  ", "Empty"))
        self.assertIsNone(server.pending_recipe_add("ac:eb:e6:c9:4f:8c", "x" * (server.PENDING_MAX_TEXT + 1)))
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Old")
        pending = json.loads(server.PENDING_RECIPES.read_text())
        pending["ACEBE6C94F8C"]["added"] = int(time.time()) - (server.PENDING_DAYS + 1) * 86400
        server.save_pending_recipes(pending)
        self.assertIsNone(server.pending_recipe_take("ACEBE6C94F8C", "mb_generic"))


class LinkHandsOverTheRecipe(unittest.TestCase):
    """link_poll: a linked board gets its recipe on its entry and in the reply."""

    def setUp(self):
        server.PENDING_RECIPES.unlink(missing_ok=True)
        server.PROXIES.unlink(missing_ok=True)
        self.saved = (server.request, server.device_info, server.proxy_name, server.dismiss, server.load_wifi_default)
        server.request = lambda *a, **k: (200, json.dumps({"state": "linked", "token": "t0ken"}))
        server.device_info = lambda address: {"id": "ACEBE6C94F8C", "app": "mb_generic", "kind": "matter-builder"}
        server.proxy_name = lambda host, token: "Matter Builder"
        server.dismiss = lambda device_id: None
        server.load_wifi_default = lambda: {}

    def tearDown(self):
        (server.request, server.device_info, server.proxy_name, server.dismiss, server.load_wifi_default) = self.saved

    def link(self):
        server.links["l1"] = {"host": "76EE82AFBAB2D1B9.local", "address": "fd7e::1", "password": "p", "id": "x"}
        return server.link_poll("l1")

    def test_recipe_on_the_linked_device(self):
        server.pending_recipe_add("ac:eb:e6:c9:4f:8c", RECIPE, "Qlima D620")
        reply = self.link()
        self.assertEqual(reply["state"], "linked")
        self.assertEqual(reply["recipe"]["name"], "Qlima D620")
        proxy = server.load_proxies()[0]
        self.assertEqual(proxy["recipe"]["text"], RECIPE)
        server.remember(proxy, recipe=None)  # what api/proxy/<id>/recipe does once applied
        self.assertIsNone(server.load_proxies()[0]["recipe"])
        self.assertEqual(server.load_pending_recipes(), {})

    def test_no_recipe_no_change(self):
        reply = self.link()
        self.assertNotIn("recipe", reply)
        self.assertNotIn("recipe", server.load_proxies()[0])


if __name__ == "__main__":
    unittest.main()
