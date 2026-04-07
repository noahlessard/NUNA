# captive-login

Automatically authenticates against the UW–Madison guest WiFi captive portal (ClearPass / Aruba). Intended to run as a background service on a headless Raspberry Pi so the device stays connected without manual intervention.

## How it works

1. Polls `connectivitycheck.gstatic.com` every 5 seconds; exits the loop if HTTP 204 is returned (open internet)
2. On captive portal detection, registers a new guest account with randomized credentials
3. Follows the portal's multi-step redirect chain to obtain the final auth token
4. POSTs credentials to the securelogin endpoint, completing authentication
5. Sleeps and repeats

## Configuration

Edit `captive-login/config.h` before building:

| Constant | Purpose |
|---|---|
| `PORTAL_INIT_URL` | ClearPass portal entry point (also used as the POST target and Referer) |
| `RECEIPT_URL` | Portal receipt page URL |
| `CONFIRM_URL` | Final securelogin endpoint |
| `USER_AGENT` | Browser User-Agent string sent with all requests |

## Building

### Native

```bash
sudo apt install cmake libcurl4-openssl-dev
cd captive-login
cmake -B build
cmake --build build
```

Output binary: `build/captive-login`

### Docker (Cross-compile from x86)

```bash
docker compose run --rm cross-compile
```

Output binary: `build-arm/captive-login`

The docker targets 32-bit ARM (`armv7l / gnueabihf`) — runs on Raspberry Pi 2/3/4/Zero 2 W.


## Debugging

If credential extraction fails, the program dumps the raw HTML to `/tmp/` on the Pi:

- `/tmp/captive-confirm.html` — confirmation page (step 3)
- `/tmp/captive-weblogin.html` — weblogin page (step 5)
