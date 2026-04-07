#include <iostream>
#include <vector>
#include <string>
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <random>
#include "config.h"

// RAII wrapper for curl easy handle
class CurlHandle {
    CURL* curl;
public:
    CurlHandle() : curl(curl_easy_init()) {
        if (!curl) throw std::runtime_error("Failed to initialize curl");
    }
    ~CurlHandle() { if (curl) curl_easy_cleanup(curl); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CURL* get() { return curl; }
};

// RAII wrapper for curl header list
class CurlHeaders {
    struct curl_slist* list = nullptr;
public:
    CurlHeaders(std::initializer_list<std::string> headers) {
        for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    }
    ~CurlHeaders() { curl_slist_free_all(list); }
    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
    struct curl_slist* get() { return list; }
};

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

#define LOG(msg) std::cout << "[" << getCurrentTimestamp() << "] " << msg << std::endl

// Apply common options shared by every handle
static void setupHandle(CURL* h, const char* url) {
    curl_easy_setopt(h, CURLOPT_URL,            url);
    curl_easy_setopt(h, CURLOPT_USERAGENT,      USER_AGENT);
    curl_easy_setopt(h, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS,     1L);
}

// Extract "https://host" from a full URL (used to build the Origin header)
static std::string originOf(const char* url) {
    std::string s(url);
    size_t slash = s.find('/', s.find("//") + 2);
    return slash != std::string::npos ? s.substr(0, slash) : s;
}

std::string generateRandomString(size_t length) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    static std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);
    std::string result(length, '\0');
    for (auto& c : result) c = chars[dist(rng)];
    return result;
}

std::string buildPostBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    CurlHandle tmp;
    std::string body;
    for (const auto& [key, val] : fields) {
        if (!body.empty()) body += "&";
        char* k = curl_easy_escape(tmp.get(), key.c_str(), 0);
        char* v = curl_easy_escape(tmp.get(), val.c_str(), 0);
        if (k && v) { body += k; body += "="; body += v; }
        curl_free(k);
        curl_free(v);
    }
    return body;
}

// Extract the value of the first input with the given name attribute
std::string extractInputValue(const std::string& html, const std::string& name) {
    for (const char q : {'"', '\''}) {
        std::string needle = std::string("name=") + q + name + q;
        size_t pos = html.find(needle);
        if (pos == std::string::npos) continue;

        std::string vd = std::string("value=") + q;
        size_t vs = html.find(vd, pos);
        if (vs == std::string::npos) continue;

        vs += vd.size();
        size_t ve = html.find(q, vs);
        if (ve != std::string::npos)
            return html.substr(vs, ve - vs);
    }
    return {};
}

bool extractLoginCredentials(const std::string& html, std::string& username, std::string& password) {
    username = extractInputValue(html, "username");
    password = extractInputValue(html, "password");

    if (username.empty() || password.empty()) {
        LOG("Credential extraction failed (html size=" << html.size() << "), dumping to /tmp/captive-confirm.html");
        FILE* f = fopen("/tmp/captive-confirm.html", "wb");
        if (f) { fwrite(html.c_str(), 1, html.size(), f); fclose(f); }
        return false;
    }
    return true;
}

void transferCookies(CurlHandle& from, CurlHandle& to) {
    curl_easy_setopt(to.get(), CURLOPT_COOKIEFILE, "");  // enable cookie engine
    struct curl_slist* cookies = nullptr;
    curl_easy_getinfo(from.get(), CURLINFO_COOKIELIST, &cookies);
    for (struct curl_slist* c = cookies; c; c = c->next)
        curl_easy_setopt(to.get(), CURLOPT_COOKIELIST, c->data);
    curl_slist_free_all(cookies);
}

static size_t writeToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t discardBody(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

bool checkConnectivity() {
    CurlHandle h;
    setupHandle(h.get(), "http://connectivitycheck.gstatic.com/generate_204");
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, discardBody);

    if (curl_easy_perform(h.get()) != CURLE_OK) return false;
    long code = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &code);
    return code == 204;
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const std::string origin        = originOf(PORTAL_INIT_URL);
    const std::string originHeader  = "Origin: "  + origin;
    const std::string refererHeader = "Referer: " PORTAL_INIT_URL;

    LOG("Captive Portal Auto-Login started");

    while (true) {
        if (checkConnectivity()) {
            LOG("Connected to internet");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        LOG("Captive portal detected");

        // Step 1: GET portal page to capture session cookie
        CurlHandle init_handle;
        setupHandle(init_handle.get(), PORTAL_INIT_URL);
        curl_easy_setopt(init_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(init_handle.get(), CURLOPT_COOKIEJAR,     "");
        curl_easy_setopt(init_handle.get(), CURLOPT_COOKIEFILE,    "");
        curl_easy_setopt(init_handle.get(), CURLOPT_WRITEFUNCTION, discardBody);

        if (curl_easy_perform(init_handle.get()) != CURLE_OK) {
            LOG("Failed to initialize session");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // Step 2: POST registration with randomized credentials
        std::string rand_str     = generateRandomString(10);
        std::string visitor_name = "user" + rand_str;
        std::string email        = rand_str + "@mailnull.com";
        LOG("Registering as " << visitor_name << " / " << email);

        std::string post_body = buildPostBody({
            {"expire_after",         "336"},
            {"role_id",              "2"},
            {"visitor_name",         visitor_name},
            {"email",                email},
            {"creator_accept_terms", "1"},
        });

        CurlHeaders reg_headers({
            "Content-Type: application/x-www-form-urlencoded",
            originHeader, refererHeader,
            "Connection: keep-alive",
            "Upgrade-Insecure-Requests: 1",
        });
        CurlHandle login_handle;
        setupHandle(login_handle.get(), PORTAL_INIT_URL);
        curl_easy_setopt(login_handle.get(), CURLOPT_POST,           1L);
        curl_easy_setopt(login_handle.get(), CURLOPT_POSTFIELDS,     post_body.c_str());
        curl_easy_setopt(login_handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(login_handle.get(), CURLOPT_HTTPHEADER,     reg_headers.get());
        curl_easy_setopt(login_handle.get(), CURLOPT_WRITEFUNCTION,  discardBody);
        transferCookies(init_handle, login_handle);

        CURLcode res = curl_easy_perform(login_handle.get());
        if (res != CURLE_OK) {
            LOG("Registration POST failed: " << curl_easy_strerror(res));
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        long code = 0;
        curl_easy_getinfo(login_handle.get(), CURLINFO_RESPONSE_CODE, &code);
        if (code != 302) {
            LOG("Registration failed, expected 302 but got: " << code);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // Step 3: GET confirmation page to extract generated credentials
        char* redir_raw = nullptr;
        curl_easy_getinfo(login_handle.get(), CURLINFO_REDIRECT_URL, &redir_raw);
        if (!redir_raw) {
            LOG("No redirect URL in 302 response");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        std::string redir_url(redir_raw);
        LOG("Redirect URL: " << redir_url);

        std::string confirm_html;
        CurlHandle confirm_handle;
        setupHandle(confirm_handle.get(), redir_url.c_str());
        curl_easy_setopt(confirm_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(confirm_handle.get(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(confirm_handle.get(), CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(confirm_handle.get(), CURLOPT_WRITEFUNCTION,  writeToString);
        curl_easy_setopt(confirm_handle.get(), CURLOPT_WRITEDATA,      &confirm_html);
        transferCookies(login_handle, confirm_handle);

        if (curl_easy_perform(confirm_handle.get()) != CURLE_OK) {
            LOG("Confirmation page fetch failed");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        LOG("Confirmation page received (" << confirm_html.size() << " bytes)");

        std::string username, password;
        if (!extractLoginCredentials(confirm_html, username, password)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        LOG("Extracted username: " << username);

        // Step 4: POST receipt form — triggers captive portal redirect to weblogin page
        std::string receipt_body = buildPostBody({
            {"visitor_name", visitor_name},
            {"username",     username},
            {"password",     password},
        });

        CurlHeaders receipt_headers({
            "Content-Type: application/x-www-form-urlencoded",
            originHeader, refererHeader,
            "Connection: keep-alive",
        });
        std::string weblogin_html;
        CurlHandle receipt_handle;
        setupHandle(receipt_handle.get(), RECEIPT_URL);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_POST,           1L);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_POSTFIELDS,     receipt_body.c_str());
        curl_easy_setopt(receipt_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_HTTPHEADER,     receipt_headers.get());
        curl_easy_setopt(receipt_handle.get(), CURLOPT_WRITEFUNCTION,  writeToString);
        curl_easy_setopt(receipt_handle.get(), CURLOPT_WRITEDATA,      &weblogin_html);
        transferCookies(confirm_handle, receipt_handle);

        if (curl_easy_perform(receipt_handle.get()) != CURLE_OK) {
            LOG("Receipt POST failed");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        LOG("Weblogin page received (" << weblogin_html.size() << " bytes)");

        // Step 5: Extract final credentials from weblogin page and POST to securelogin.
        // The portal page normally auto-submits via JS; we do it directly.
        std::string login_user = extractInputValue(weblogin_html, "user");
        std::string login_pass = extractInputValue(weblogin_html, "password");

        if (login_user.empty() || login_pass.empty()) {
            LOG("Failed to extract weblogin credentials, dumping to /tmp/captive-weblogin.html");
            FILE* f = fopen("/tmp/captive-weblogin.html", "wb");
            if (f) { fwrite(weblogin_html.c_str(), 1, weblogin_html.size(), f); fclose(f); }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::string auth_body = buildPostBody({
            {"user",     login_user},
            {"password", login_pass},
            {"cmd",      "authenticate"},
            {"url",      "http://www.msftconnecttest.com/redirect"},
            {"Login",    "Log In"},
        });

        CurlHeaders auth_headers({
            "Content-Type: application/x-www-form-urlencoded",
            originHeader,
            "Connection: keep-alive",
        });
        CurlHandle auth_handle;
        setupHandle(auth_handle.get(), CONFIRM_URL);
        curl_easy_setopt(auth_handle.get(), CURLOPT_POST,           1L);
        curl_easy_setopt(auth_handle.get(), CURLOPT_POSTFIELDS,     auth_body.c_str());
        curl_easy_setopt(auth_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(auth_handle.get(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(auth_handle.get(), CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(auth_handle.get(), CURLOPT_HTTPHEADER,     auth_headers.get());
        curl_easy_setopt(auth_handle.get(), CURLOPT_WRITEFUNCTION,  discardBody);

        res = curl_easy_perform(auth_handle.get());
        if (res != CURLE_OK) {
            LOG("Authentication POST failed: " << curl_easy_strerror(res));
        } else {
            curl_easy_getinfo(auth_handle.get(), CURLINFO_RESPONSE_CODE, &code);
            LOG("Authentication completed with response code: " << code);
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    return 0;
}
