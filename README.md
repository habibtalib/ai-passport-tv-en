> **English-only edition.** This fork translates the firmware UI, server messages and docs into English. Original project: [guolifu/ai-passport-tv](https://github.com/guolifu/ai-passport-tv).

# Passport TV

Turn a FoloToy AI Passport into a small live-TV receiver. A Mac or Linux computer fetches and transcodes live channels; the device receives and plays them over Wi-Fi. Three buttons control channels, volume, brightness and quality.

**Standalone firmware and service, with no FoloTalk account or Admin dependency.** FoloTalk can continue to embed the same player and Wi-Fi components.

- ESP32-C3, 8 MB Flash, no PSRAM.
- 320×180 RGB332, complete-frame presentation; 16 kHz mono ADPCM audio.
- Smooth / Standard / Ultra: 8 / 15 / 30 fps; audio format stays the same.
- One shared live encoder per channel/profile, with bounded queues per viewer.
- Source reconnection shows 4 fps monochrome TV static with `RECONNECTING...`; normal video resumes automatically, without adding static noise to the audio.
- **macOS and Linux** service launcher: virtual environment, Python dependencies, pairing code and first-channel setup.

```text
Live source → Mac / Linux: FFmpeg + TV service → Wi-Fi → AI Passport
```

## 1. Get the project

```sh
git clone https://github.com/habibtalib/ai-passport-tv-en.git
cd ai-passport-tv-en
```

| Path | Purpose |
| --- | --- |
| `Start TV.command` | Double-click entry for Mac |
| `start-server.sh` | Terminal entry for Mac/Linux |
| `tools/` | Launcher, firmware build and verification tools |
| `firmware/` | Independent ESP-IDF project and shared components |
| `service/` | Python TV service and tests |
| `stream.env.example` | Accepted playback configuration template |
| `channels.example.txt` | Default CCTV1 example including its required User-Agent |
| `fonts/` | OFL license and standalone font provenance |

## 2. Install service prerequisites

**ESP-IDF is not needed to run the server.** Install Python 3.10+, FFmpeg and ffprobe, available on PATH.

macOS, after installing [Homebrew](https://brew.sh/):

```sh
brew install python ffmpeg
```

Ubuntu 22.04+ / Debian 12+:

```sh
sudo apt-get update
sudo apt-get install python3 python3-venv python3-pip ffmpeg
```

Other Linux distributions: install equivalent packages using your package manager. Check:

```sh
python3 --version
ffmpeg -version
ffprobe -version
```

Official references: [FFmpeg downloads](https://ffmpeg.org/download.html), [Homebrew FFmpeg](https://formulae.brew.sh/formula/ffmpeg).

## 3. Start the service

**Mac:** double-click `Start TV.command` in Finder. Alternatively, on Mac or Linux, run from the repository root:

```sh
./start-server.sh
```

On first run the launcher checks prerequisites, creates `server.env`, `channels.txt`, a four-character `pairing.code` and a private signing secret in `pairing.token`, uses the supplied CCTV1 example (asks for a channel name and URL only when the list is empty), creates `.venv`, installs `service/requirements.txt`, and starts the listener. Existing files are never replaced. The first dependency installation needs Internet access; unchanged, healthy dependencies are reused afterwards.

The launcher does not install system packages, register background services or change firewall rules. Keep the terminal open; `Ctrl+C` stops playback. Restart the same entry to reuse settings. Keep a Mac awake while serving.

If a ZIP download loses executable permissions:

```sh
chmod +x start-server.sh 'Start TV.command'
```

Optional commands:

```sh
./start-server.sh --setup-only     # Prepare configuration/dependencies without starting
./start-server.sh --check          # Validate only; no installation or listener
./start-server.sh --show-pairing   # Display the pairing code locally
```

For unattended use, fill in `channels.txt` first. A noninteractive launch without a channel exits clearly rather than waiting for input. Select an interpreter with `PYTHON=/path/to/python3 ./start-server.sh` if necessary.

### Channels

Edit `channels.txt` in the repository root:

```text
# id | Display name | HTTP/HTTPS live URL [| optional User-Agent]
news | News | https://example.com/live/news.m3u8
sports | Sports | https://example.com/live/sports.m3u8 | MyPlayer/1.0
```

The two example.com URLs above illustrate syntax only. `channels.example.txt` supplies a CCTV1 entry with the required `AptvPlayer-UA`. First startup copies it into local `channels.txt`, preserving any existing file, so you can test CCTV1 before adding other authorized sources. Third-party availability can change; replace the URL if needed. IDs must be unique printable ASCII without spaces, at most 15 bytes. Names may contain Chinese. The device receives the first 40 channels by default; the first channel is the initial default. Restart after edits. Keep credential-bearing source URLs private.

This is a live-TV application, with no bundled films or per-viewer VOD progress management.

### Server and playback configuration

Edit generated `server.env`, one `TV_NAME=value` per line, then restart. It is parsed as data, never executed as a shell script. Precedence: process environment > `server.env` > `stream.env.example`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `TV_BIND` | `0.0.0.0` | Listen on all local IPv4 interfaces; `127.0.0.1` is local-only |
| `TV_PORT` | `10745` | Device-facing TCP port |
| `TV_FPS` | `30` | Retain Ultra target; devices can still select 8/15/30 profiles |
| `TV_ADAPTIVE` | `0` | No adaptive budget reduction |
| `TV_RATE_START/MIN/MAX` | `1000000` each | Fixed send budget in bytes/second |
| `TV_PACKET_TARGET` | `12288` | 12 KiB media-packet target |
| `TV_ENCODE` | `full` | Accepted full-frame encoding path |
| `TV_WRITE_PACING_RATE` | `1000000` | Userspace write pacing in bytes/second |
| `TV_WRITE_PACING_CHUNK` | `12288` | Maximum paced write batch |
| `TV_TCP_PACING_RATE` | Linux `1100000` | Linux kernel pacing; launcher forces `0` on Mac |
| `TV_MAX_SESSIONS` | `2` | Total connection limit |
| `TV_MAX_PER_ADDRESS` | `2` | Connection limit per source IP |
| `TV_SHARED_LIVE` | `1` | Share encoding for the same channel/profile |

On Mac only the unsupported Linux kernel option is disabled. **Userspace pacing, image format, frame targets, audio and send budget remain unchanged.** Actual frame rate depends on the source, host performance and network.

The launcher generates a four-character alphanumeric **pairing code** in `pairing.code`. It is case-insensitive, reusable by multiple devices and **does not expire by default**. Restarting the service preserves it. Display it locally with `./start-server.sh --show-pairing`; keep mode `0600` when editing it.

The long random value in `pairing.token` is an internal signing secret, not the code entered in the setup page. Keep it and `paired-devices.json` private and preserve both when migrating the server. Existing devices configured with the old long token can continue playing. Changing the short code prevents new pairing with the old code; existing device bindings remain. Rotating the signing secret invalidates issued playback credentials. Do not also set `TV_PAIRING_TOKEN`.

Pairing and configuration requests use HTTP on the same TCP port as TV playback. HTTPS provisioning origins are supported by the device when a deployment supplies a gateway. For a public port mapping or gateway whose address differs from the TV listener, configure `TV_PUBLIC_HOST` and `TV_PUBLIC_PORT` as the device-facing playback endpoint.

`server.env`, `channels.txt`, `pairing.code`, `pairing.token`, `paired-devices.json` and `.venv` are Git-ignored.

## 4. Build firmware

Use **ESP-IDF v5.5.3**. Install system prerequisites from [Espressif's Mac/Linux setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/linux-macos-setup.html), then install and activate the toolchain:

```sh
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3
. ./export.sh
```

Return to this repository's root:

```sh
./tools/build.sh
```

The first build downloads the components pinned by `dependencies.lock`. It uses a separate `build/sdkconfig` and needs no FoloTalk invitation, Wi-Fi password or server credential.

| Output | Purpose |
| --- | --- |
| `build/FoloToy-AI-Passport-full.bin` | Merged image for fresh setup |
| `build/FoloToy-AI-Passport.bin` | Application-partition image |
| `dist/firmware/<full-image-SHA256>/` | Images, matching ELF/MAP, partition table and manifest |

The script verifies partition bounds, merged image and archive. Recheck an archive with:

```sh
python3 tools/archive_firmware.py verify dist/firmware/<full-image-SHA256>
```

### Flashing and existing settings

Connect the intended AI Passport with a data cable. Mac ports commonly appear as `/dev/cu.usbmodem...`, Linux as `/dev/ttyACM...`. Replace `PORT` below with the identified device's actual port.

**For fresh setup, where resetting settings is acceptable**, use the merged image from an activated ESP-IDF environment:

```sh
python -m esptool --chip esp32c3 --port PORT write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

**To retain an existing FoloTalk account/settings, do not use the merged-image command.** First confirm the device's existing partition table exactly matches `firmware/partitions.csv`; only then update the application:

```sh
python -m esptool --chip esp32c3 --port PORT write_flash 0x10000 build/FoloToy-AI-Passport.bin
```

Baseline: NVS `0x9000/0x6000`, app `0x10000/0x76d000`, photo `0x77d000/0x80000`, recovery `0x7fd000/0x3000`. Do not flash an unverified layout; whole-chip erase is unnecessary. Standalone imports remembered Wi-Fi read-only, stores its own connection in `ptv_config`, issuer-scoped credentials in `ptv_auth`, and shares quality preference `ft_tv`; account/photo/recovery data is not changed.

## 5. Configure device Wi-Fi and TV

1. When setup opens, the screen shows a **password-free** `PassportTV-xxxx-xxx` hotspot.
2. Join it from a phone. Stay connected despite the “no Internet” notice, then open `http://192.168.4.1` manually.
3. Scan/select a **2.4 GHz** network, enter the router password and wait for connection.
4. Continue directly to TV settings, without nickname/avatar steps:

   | Field | Value |
   | --- | --- |
   | TV server | Computer LAN IP, e.g. `192.168.1.100`; an HTTP/HTTPS provisioning origin without a path is also supported |
   | Port | `10745`, matching `TV_PORT` |
   | Pairing code | Four characters from `./start-server.sh --show-pairing` |

5. Save to close setup and start playback. Reboot reconnects automatically. An already-connected device may proceed directly to TV settings.

**Use the server computer's address, not `192.168.4.1`, `0.0.0.0` or `localhost`.** A computer on 5 GHz and a device on 2.4 GHz can communicate on the same LAN if guest/AP isolation is off. Allow inbound TCP `10745` through the host firewall. A DHCP reservation helps keep the host address stable.

For a remote Linux host, enter its reachable public address/domain and external mapped TCP port; allow that port in its firewall/security group. No Admin is required. FAV1/TCP is plaintext; pairing authenticates access without encryption. Use a controlled network or VPN for remote access.

## 6. Buttons

Use these playback shortcuts with menus closed:

- Short OK: TV menu with separate Channel and Quality entries; UP/DOWN navigate, OK selects. Quality always takes OK → DOWN → OK, without scrolling through channels.
- The TV menu, channel list and quality page show a Wi-Fi signal icon, a battery gauge with percentage, FPS and receive rate in the corners, refreshed once per second. Battery levels at 20% or below turn amber, and at 10% or below turn red.
- The channel list retains its selection and scroll position. Long OK returns to the TV menu, then closes it. Quality profiles are Smooth 8 fps, Standard 15 fps and Ultra 30 fps.
- **Double OK: brightness page**; short UP/DOWN increases/decreases brightness, short OK returns to playback. Brightness is outside the channel menu.
- Short UP/DOWN: switch channels.
- Long UP/DOWN: increase/decrease volume in 10% steps.
- Double UP or DOWN: playback status.
- Long OK on the brightness/status page: close it and return to playback.
- Long OK during playback: pause/leave; short OK on the pause screen resumes.
- Long OK on the pause screen: configure Wi-Fi/server/pairing again. An empty pairing field retains an existing binding only for the same service; changing service requires its pairing code.

## 7. Troubleshooting

| Symptom | Check |
| --- | --- |
| Python/FFmpeg missing | Install section 2 prerequisites; Finder launcher adds common Homebrew paths |
| Virtual environment creation fails | Install Linux `python3-venv`; select Python 3.10+ |
| First dependency installation is slow | NumPy needs downloading once; configure a pip mirror if needed |
| No channels | Copy the CCTV1 entry from `channels.example.txt` into local `channels.txt`, or add another live source |
| Device asks to reconnect | Server still running, correct address/TCP port, matching pairing code; inspect terminal errors |
| Local access works, device fails | Listen beyond `127.0.0.1`; check firewall, guest isolation and NAT mapping |
| Source returns 403 / timeout | Check source URL and optional fourth-field User-Agent from the server |
| Mac reports Linux pacing error | Use the launcher; manual start requires `TV_TCP_PACING_RATE=0` |
| Configuration edits have no effect | Restart server and re-enter TV on the device |
| Device does not reach 30 fps | Select Ultra; source rate, transcoding and network determine actual rate |

## Development and embedding

```sh
python3 -m unittest discover -s tests -v
python3 tests/smoke_server.py
cd service
../.venv/bin/python -m unittest discover -s tests -v
```

`firmware/components/passport_tv` is the common player; `passport_wifi` owns the common connection/open-AP lifecycle. FoloTalk retains its accounts, signed tickets, viewing status and memory handoff. The standalone service uses a separate pairing entry and the same media/transcoding/sending implementation. Standalone links no social or radio code.

ED48 firmware checks covered actual open-AP browser setup, saved-network/quality reboot, pause/resume and profile switching. Device counters do not replace external optical/acoustic capture or acceptance on every network. Launcher/service checks apply to the current commit. Production configuration, private films and credentials are not included.

## License

Code retains MIT attribution. TV-service upstream revision and license: `service/UPSTREAM_COMMIT`, `service/LICENSE.upstream`. Standalone menu bitmaps derive from Adobe Source Han Sans under OFL-1.1; provenance/hashes and license: `fonts/manifest.json`, `fonts/LICENSE.txt`. FFmpeg is installed separately and follows the license of the user's FFmpeg build.

### Pairing interface

Public and private deployments use the same device request contract. The setup page has one **Pairing code** field; internal device credentials and playback tickets are never shown there.

- `POST /social/tv/activate`: JSON `{"device":"<12 lowercase hex hardware ID>","code":"<4 alphanumeric characters>"}`.
- `POST /social/tv/config`: JSON `{"device":"<hardware ID>"}`.
- Both send `Authorization: Bearer <64 lowercase hex device credential>` and receive JSON `{"host":"<TV host>","port":10745,"ticket":"<playback credential>"}`.

The device persists a random credential before pairing so a lost success response can be recovered without redeeming again. It refreshes the connection information before opening each playback session, including channel/quality switches and reconnects; authorization does not run per video frame. The common player exposes an optional connection-provider callback. FoloTalk and legacy static-token callers can omit it. Private builds select their provisioning origin through build configuration and use the same app/client sources, without editing the player at build time.

Only the server determines pairing policy: the included open-source server defaults to a permanent reusable code and nonexpiring device grants; a private backend can keep one-use codes, expiry and device binding rules. Its Admin, signing keys, databases and proprietary policy implementation are not included in this export. HTTP/FAV1 traffic is plaintext; use a trusted LAN, VPN or HTTPS gateway where appropriate.
