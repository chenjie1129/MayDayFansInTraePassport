#pragma once

// Starts the observational FAP_SCREENSHOT_V1 serial capture endpoint.
// The task remains idle until the official publisher sends a request.
void publisher_capture_start(void);
