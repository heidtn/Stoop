#pragma once

// Registers all HTTP routes for the WiFi captive portal / #stoop web UI on the
// global `server`, and starts it. Expects rate_limiter and the_mesh to already
// be initialized.
void configureServer();
