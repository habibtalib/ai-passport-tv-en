# Shared Passport Wi-Fi lifecycle

Extracted from FoloTalk's existing station/provisioning lifecycle. Both hosts use
this implementation for initialization, full scan, strongest saved-network
selection, hidden-network fallback, connection timeout/error cleanup and live
SSID/RSSI/IP snapshots. The component does not load or save credentials.
FoloTalk's wrappers retain their existing history/configuration save policy.
The standalone host reads remembered networks without writing FoloTalk keys and
saves its own successful connection only in `ptv_config`.

Both hosts call the same `WIFI_AUTH_OPEN` setup-AP function. Setup HTTP requests
remain restricted to the setup interface and matching Origin.

Provisioning preparation preserves an existing station connection. FoloTalk's
provisioning manager retains Wi-Fi start/stop ownership; standalone setup uses
the same open-AP helper. Optional automatic reconnect defaults off
and is enabled explicitly by the standalone host after a successful connection.
