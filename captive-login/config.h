#pragma once

#define PORTAL_INIT_URL  "https://clearpass.uwnet.wisc.edu/guest/portal-mm.php?_browser=1"
#define RECEIPT_URL      "https://clearpass.uwnet.wisc.edu/guest/portal-mm_receipt.php"
#define CONFIRM_URL      "https://securelogin.local.uwnet.wisc.edu/cgi-bin/login"
#define USER_AGENT       "Mozilla/5.0 (X11; Linux x86_64; rv:147.0) Gecko/20100101 Firefox/147.0"

// Timing
#define CHECK_INTERVAL_SECONDS 30
#define MAX_AUTH_FAILURES      3

// UW-NET profile name (match nmcli connection name)
#define WIFI_SSID "UWNET"
