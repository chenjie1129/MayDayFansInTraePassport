import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PORTAL_SOURCE = (ROOT / "main" / "wifi_portal.c").read_text(encoding="utf-8")
PORTAL_PAGE = (ROOT / "main" / "wifi_portal.html").read_text(encoding="utf-8")
WIFI_UI = (ROOT / "main" / "demo_wifi.c").read_text(encoding="utf-8")
PLACES = (ROOT / "main" / "demo_place.c").read_text(encoding="utf-8")
KCONFIG = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
SDKCONFIG_DEFAULTS = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")


class WifiPortalContractTest(unittest.TestCase):
    def test_credentials_stay_in_ram_and_are_zeroed(self):
        self.assertIn("esp_wifi_set_storage(WIFI_STORAGE_RAM)", PORTAL_SOURCE)
        self.assertNotIn("WIFI_STORAGE_FLASH", PORTAL_SOURCE)
        self.assertIn("secure_zero(&s_pending_config", PORTAL_SOURCE)
        self.assertIn("secure_zero(&station_config", PORTAL_SOURCE)
        self.assertIn("free_connect_request_buffers(buffers)", PORTAL_SOURCE)

    def test_shared_nvs_is_never_erased(self):
        self.assertNotIn("nvs_flash_erase", PORTAL_SOURCE)

    def test_setup_access_point_is_not_open(self):
        self.assertIn("WIFI_AUTH_WPA2_PSK", PORTAL_SOURCE)
        self.assertIn("get_setup_password(", PORTAL_SOURCE)
        self.assertNotIn('#define SETUP_PASSWORD "wifi-setup"', PORTAL_SOURCE)

    def test_fixed_test_password_requires_explicit_build_option(self):
        self.assertIn("config WIFI_PORTAL_FIXED_TEST_PASSWORD", KCONFIG)
        self.assertIn("default n", KCONFIG)
        self.assertIn(
            "#if CONFIG_WIFI_PORTAL_FIXED_TEST_PASSWORD",
            PORTAL_SOURCE,
        )
        self.assertIn('"88888888"', PORTAL_SOURCE)
        self.assertIn(
            "CONFIG_WIFI_PORTAL_FIXED_TEST_PASSWORD=y",
            SDKCONFIG_DEFAULTS,
        )

    def test_password_is_not_logged(self):
        log_lines = [
            line.lower()
            for line in PORTAL_SOURCE.splitlines()
            if re.search(r"\b(?:ESP_LOG[A-Z]+|printf)\s*\(", line)
        ]
        self.assertFalse(
            any("password" in line for line in log_lines),
            log_lines,
        )

    def test_portal_posts_credentials_and_clears_password(self):
        self.assertIn('method: "POST"', PORTAL_PAGE)
        self.assertIn("HTTP_POST", PORTAL_SOURCE)
        self.assertIn('password.value = "";', PORTAL_PAGE)
        self.assertIn('apiKey.value = "";', PORTAL_PAGE)

    def test_connected_session_persists_after_page_exit(self):
        self.assertIn("wifi_portal_should_persist()", WIFI_UI)
        self.assertIn("wifi_portal_begin_setup()", WIFI_UI)
        self.assertIn("snapshot->target_ssid", WIFI_UI)
        self.assertIn("snapshot->ip", WIFI_UI)
        self.assertIn("snapshot->rssi", WIFI_UI)

    def test_explicit_stop_waits_for_radio_cleanup(self):
        stop_call = WIFI_UI.index("wifi_portal_stop();")
        screen_delete = WIFI_UI.index("lv_obj_delete(s_scr);")
        self.assertLess(stop_call, screen_delete)

        cleanup = PORTAL_SOURCE[
            PORTAL_SOURCE.index("static void network_cleanup(void)") :
            PORTAL_SOURCE.index("static bool stop_requested(void)")
        ]
        self.assertLess(
            cleanup.index("stop_portal_services();"),
            cleanup.index("esp_wifi_stop();"),
        )
        self.assertLess(
            cleanup.index("esp_wifi_stop();"),
            cleanup.index("esp_wifi_deinit();"),
        )
        self.assertIn("esp_netif_destroy_default_wifi(s_sta_netif)", cleanup)
        self.assertIn("esp_netif_destroy_default_wifi(s_ap_netif)", cleanup)

    def test_places_keeps_its_bounded_scan_lifecycle(self):
        self.assertIn("bsp_wifi_scan_once(", PLACES)
        self.assertNotIn("wifi_portal_start(", PLACES)
        self.assertIn("wifi_portal_suspend();", PLACES)
        self.assertIn("wifi_portal_resume();", PLACES)

    def test_connect_handler_keeps_large_secret_buffers_off_http_stack(self):
        self.assertIn(
            "connect_request_buffers_t *buffers = calloc(",
            PORTAL_SOURCE,
        )
        self.assertIn("config.stack_size = 6144;", PORTAL_SOURCE)
        handler = PORTAL_SOURCE[
            PORTAL_SOURCE.index("static esp_err_t connect_post_handler") :
            PORTAL_SOURCE.index("static esp_err_t portal_redirect_handler")
        ]
        self.assertNotIn("char body[1024]", handler)
        self.assertNotIn("char api_key[", handler)


if __name__ == "__main__":
    unittest.main()
