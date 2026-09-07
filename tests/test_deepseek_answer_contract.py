import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CLIENT = (ROOT / "main" / "deepseek_answer.c").read_text(encoding="utf-8")
SESSION = (ROOT / "main" / "wifi_session.c").read_text(encoding="utf-8")
PORTAL = (ROOT / "main" / "wifi_portal.c").read_text(encoding="utf-8")
PAGE = (ROOT / "main" / "wifi_portal.html").read_text(encoding="utf-8")
ANSWER_UI = (ROOT / "main" / "demo_answer.c").read_text(encoding="utf-8")
BSP_AUDIO = (
    ROOT / "components" / "bsp" / "src" / "bsp_audio.c"
).read_text(encoding="utf-8")
SDKCONFIG_DEFAULTS = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")


class DeepSeekAnswerContractTest(unittest.TestCase):
    def test_api_key_is_masked_and_cleared_after_submission(self):
        self.assertIn('id="apiKey"', PAGE)
        self.assertIn('name="api_key" type="password"', PAGE)
        self.assertIn('apiKey.value = "";', PAGE)
        self.assertIn("secure_zero(api_key", PORTAL)
        self.assertIn("secure_zero(s_api_key", CLIENT)

    def test_api_key_is_never_persisted_or_logged(self):
        combined = SESSION + PORTAL + CLIENT
        self.assertNotIn("nvs_set_", combined)
        log_lines = [
            line.lower()
            for line in combined.splitlines()
            if re.search(r"\b(?:ESP_LOG[A-Z]+|printf)\s*\(", line)
        ]
        self.assertFalse(any("api_key" in line for line in log_lines))
        self.assertFalse(any("authorization" in line for line in log_lines))

    def test_deepseek_request_uses_tls_and_bearer_auth(self):
        self.assertIn(
            '"https://api.deepseek.com/chat/completions"',
            CLIENT,
        )
        self.assertIn("esp_crt_bundle_attach", CLIENT)
        self.assertIn('"Authorization"', CLIENT)
        self.assertIn('"Bearer %s"', CLIENT)

    def test_transport_timeouts_are_classified_and_diagnosable(self):
        self.assertIn("#define REQUEST_TIMEOUT_MS 60000", CLIENT)
        self.assertIn("#define STOP_TIMEOUT_MS    65000", CLIENT)
        self.assertIn("ESP_ERR_HTTP_CONNECTING", CLIENT)
        self.assertIn("ESP_ERR_HTTP_WRITE_DATA", CLIENT)
        self.assertIn("ESP_ERR_HTTP_EAGAIN", CLIENT)
        self.assertIn("ESP_ERR_HTTP_READ_TIMEOUT", CLIENT)
        self.assertIn("esp_http_client_get_errno(client)", CLIENT)
        self.assertIn(
            "esp_http_client_get_and_clear_last_tls_error(",
            CLIENT,
        )

    def test_model_output_is_bounded_to_local_answer_bank(self):
        self.assertIn("answer->valueint >= 0 && answer->valueint < 16", CLIENT)
        self.assertIn("reveal(network.answer_index)", ANSWER_UI)
        self.assertNotIn("content->valuestring);", ANSWER_UI)

    def test_answer_prompts_for_missing_network_and_key(self):
        self.assertIn("OPEN WI-FI APP FIRST", ANSWER_UI)
        self.assertIn("ADD API KEY IN WI-FI APP", ANSWER_UI)
        self.assertIn("DEEPSEEK_ANSWER_NEEDS_WIFI", CLIENT)
        self.assertIn("DEEPSEEK_ANSWER_NEEDS_API_KEY", CLIENT)

    def test_answer_exit_stops_request_worker_before_deleting_ui(self):
        stop_call = ANSWER_UI.index("deepseek_answer_stop();")
        screen_delete = ANSWER_UI.index("lv_obj_delete(s_scr);")
        self.assertLess(stop_call, screen_delete)
        self.assertIn("wifi_portal_resume()", CLIENT)
        self.assertIn("wifi_portal_get_snapshot(&network)", CLIENT)
        self.assertNotIn("esp_wifi_init(", CLIENT)
        self.assertNotIn("esp_wifi_stop(", CLIENT)
        self.assertNotIn("esp_wifi_deinit(", CLIENT)

    def test_audio_does_not_compete_with_tls_request_memory(self):
        enter = ANSWER_UI[
            ANSWER_UI.index("void demo_answer_enter(void)") :
            ANSWER_UI.index("void demo_answer_exit(void)")
        ]
        self.assertNotIn("xTaskCreate(", enter)
        self.assertNotIn("bsp_audio_set_format(", enter)
        self.assertIn("play_reveal_sound();", ANSWER_UI)
        self.assertIn("bsp_audio_close();", ANSWER_UI)
        self.assertIn("esp_codec_dev_close(s_dev)", BSP_AUDIO)
        self.assertIn("CONFIG_MBEDTLS_DYNAMIC_BUFFER=y", SDKCONFIG_DEFAULTS)


if __name__ == "__main__":
    unittest.main()
