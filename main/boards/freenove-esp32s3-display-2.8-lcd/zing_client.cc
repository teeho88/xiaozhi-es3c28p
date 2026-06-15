#include "zing_client.h"

#include "board.h"
#include "network_interface.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <miniz.h>  // ROM tinfl: zingmp3.vn always gzips, even for Accept-Encoding: identity

#include <cstring>
#include <ctime>
#include <memory>

#define TAG "ZingClient"

namespace {

// Long-stable community keys. `version` is a free parameter — the server only
// recomputes the signature from the exact params we send, so its value is
// arbitrary as long as it matches between the hashed string and the query.
constexpr const char* kBase = "https://zingmp3.vn";
constexpr const char* kApiKey = "88265e23d4284f25963e6eedac8fbfa3";
constexpr const char* kSecretKey = "2aa2d1c561e809b267f3638c4a307aab";
constexpr const char* kVersion = "1.13.6";
constexpr int kConnectId = 6;  // distinct from video(0/1), pcm-audio(4), weather(5)

// Cached cookie from the first homepage GET (some endpoints want it).
std::string g_cookie;

std::string ToHex(const unsigned char* buf, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[buf[i] >> 4]);
        out.push_back(hex[buf[i] & 0x0F]);
    }
    return out;
}

std::string Sha256Hex(const std::string& in) {
    unsigned char h[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(in.data()), in.size(), h, 0);
    return ToHex(h, sizeof(h));
}

std::string HmacSha512Hex(const std::string& key, const std::string& msg) {
    unsigned char h[64];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (info == nullptr ||
        mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                        reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), h) != 0) {
        return "";
    }
    return ToHex(h, sizeof(h));
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

void CaptureCookie(Http* http) {
    if (!g_cookie.empty()) {
        return;
    }
    // GetResponseHeader returns every Set-Cookie header joined by '\n' (see the
    // accumulation in HttpClient::ParseHeaderLine). Each looks like
    // "name=value; attr; attr". Keep just the "name=value" of each and rejoin
    // into a single "n1=v1; n2=v2" Cookie string -- Zing requires zmp3_rqid, and
    // the server's cookie ordering is not guaranteed, so we send all of them.
    std::string set_cookie = http->GetResponseHeader("Set-Cookie");
    std::string combined;
    size_t start = 0;
    while (start <= set_cookie.size()) {
        size_t nl = set_cookie.find('\n', start);
        size_t line_end = (nl == std::string::npos) ? set_cookie.size() : nl;
        std::string one = set_cookie.substr(start, line_end - start);
        size_t semi = one.find(';');
        std::string nv = (semi == std::string::npos) ? one : one.substr(0, semi);
        // Trim surrounding whitespace.
        size_t b = nv.find_first_not_of(" \t\r");
        size_t e = nv.find_last_not_of(" \t\r");
        if (b != std::string::npos && nv.find('=') != std::string::npos) {
            nv = nv.substr(b, e - b + 1);
            if (!combined.empty()) {
                combined += "; ";
            }
            combined += nv;
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    g_cookie = combined;
}

// Inflate a gzip body into a fresh PSRAM buffer. Returns the malloc'd buffer
// (caller heap_caps_free) with *out_len bytes + a trailing NUL, or nullptr on
// failure. zingmp3.vn serves gzip unconditionally (it ignores our
// Accept-Encoding: identity), and the HTTP client does not decompress, so the
// raw body is a gzip stream that cJSON cannot parse without this step.
char* GzipInflate(const char* in, size_t in_len, size_t* out_len) {
    *out_len = 0;
    // gzip header: magic 1f 8b, CM=08 (deflate); need at least header(10)+trailer(8).
    if (in_len < 18 || static_cast<uint8_t>(in[0]) != 0x1f ||
        static_cast<uint8_t>(in[1]) != 0x8b || static_cast<uint8_t>(in[2]) != 0x08) {
        return nullptr;
    }
    uint8_t flg = static_cast<uint8_t>(in[3]);
    size_t pos = 10;
    if (flg & 0x04) {  // FEXTRA
        if (pos + 2 > in_len) return nullptr;
        size_t xlen = static_cast<uint8_t>(in[pos]) |
                      (static_cast<size_t>(static_cast<uint8_t>(in[pos + 1])) << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) { while (pos < in_len && in[pos] != 0) pos++; pos++; }  // FNAME
    if (flg & 0x10) { while (pos < in_len && in[pos] != 0) pos++; pos++; }  // FCOMMENT
    if (flg & 0x02) pos += 2;                                               // FHCRC
    if (pos + 8 > in_len) return nullptr;

    // ISIZE (gzip trailer, little-endian, mod 2^32) sizes the output exactly.
    uint32_t isize = static_cast<uint8_t>(in[in_len - 4]) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(in[in_len - 3])) << 8) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(in[in_len - 2])) << 16) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(in[in_len - 1])) << 24);
    size_t out_cap = isize ? isize : (in_len * 8);
    auto* out = static_cast<char*>(
        heap_caps_malloc(out_cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out == nullptr) return nullptr;

    auto* dec = static_cast<tinfl_decompressor*>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dec == nullptr) { heap_caps_free(out); return nullptr; }
    tinfl_init(dec);

    size_t in_avail = in_len - pos - 8;  // raw deflate payload (exclude 8-byte trailer)
    size_t out_avail = out_cap;
    tinfl_status st = tinfl_decompress(
        dec, reinterpret_cast<const mz_uint8*>(in + pos), &in_avail,
        reinterpret_cast<mz_uint8*>(out), reinterpret_cast<mz_uint8*>(out), &out_avail,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(dec);
    if (st != TINFL_STATUS_DONE) {
        heap_caps_free(out);
        return nullptr;
    }
    out[out_avail] = '\0';
    *out_len = out_avail;
    return out;
}

// One GET attempt. Buffers the body in PSRAM (Zing search responses can be tens
// of KB and internal RAM is near-exhausted). `*transient` is set true when the
// attempt failed in a way worth retrying (TLS/socket read error or a truncated
// transfer) vs a definitive answer (non-200, or parse of a complete body).
// Returns the parsed cJSON root (caller cJSON_Delete) or nullptr.
cJSON* HttpGetJsonOnce(const std::string& url, bool* transient) {
    *transient = false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    if (!http) {
        *transient = true;
        return nullptr;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->SetHeader("Referer", std::string(kBase) + "/");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");  // no gzip; we don't decompress
    if (!g_cookie.empty()) {
        http->SetHeader("Cookie", g_cookie);
    }
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "HTTP open failed");
        *transient = true;  // connect/handshake failure -> retry
        return nullptr;
    }
    int status = http->GetStatusCode();  // blocks until response headers are parsed
    CaptureCookie(http.get());            // ...so Set-Cookie is available here
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        http->Close();
        return nullptr;  // definitive: don't retry a 4xx/5xx
    }

    constexpr size_t kMaxBody = 1024 * 1024;
    size_t cap = 32 * 1024;
    size_t len = 0;
    auto* buf = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        http->Close();
        return nullptr;
    }
    char tmp[1024];
    bool overflow = false;
    bool read_error = false;
    while (true) {
        int r = http->Read(tmp, sizeof(tmp));
        if (r < 0) {
            read_error = true;  // TLS/socket error mid-transfer (e.g. -0x004C)
            break;
        }
        if (r == 0) {
            break;  // clean end of body
        }
        if (len + static_cast<size_t>(r) + 1 > cap) {
            size_t ncap = cap * 2;
            while (len + static_cast<size_t>(r) + 1 > ncap) {
                ncap *= 2;
            }
            if (ncap > kMaxBody) {
                overflow = true;
                break;
            }
            auto* nb = static_cast<char*>(
                heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (nb == nullptr) {
                overflow = true;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, tmp, r);
        len += r;
    }
    http->Close();

    cJSON* root = nullptr;
    if (read_error) {
        // The body is truncated -- do NOT parse the partial buffer.
        ESP_LOGE(TAG, "read error after %u bytes (TLS/socket); will retry",
                 static_cast<unsigned>(len));
        *transient = true;
    } else if (overflow) {
        ESP_LOGE(TAG, "oversized body (len=%u)", static_cast<unsigned>(len));
    } else if (len == 0) {
        ESP_LOGE(TAG, "empty body");
        *transient = true;
    } else {
        buf[len] = '\0';
        // zingmp3.vn returns gzip even when we ask for identity; inflate first.
        char* json = buf;
        size_t json_len = len;
        char* inflated = nullptr;
        if (len >= 2 && static_cast<uint8_t>(buf[0]) == 0x1f &&
            static_cast<uint8_t>(buf[1]) == 0x8b) {
            size_t ilen = 0;
            inflated = GzipInflate(buf, len, &ilen);
            if (inflated == nullptr) {
                ESP_LOGE(TAG, "gzip inflate failed (%u bytes)", static_cast<unsigned>(len));
            } else {
                json = inflated;
                json_len = ilen;
            }
        }
        root = cJSON_Parse(json);
        if (root == nullptr) {
            ESP_LOGE(TAG, "JSON parse failed (%u bytes)", static_cast<unsigned>(json_len));
        }
        if (inflated != nullptr) {
            heap_caps_free(inflated);
        }
    }
    heap_caps_free(buf);
    return root;
}

// GET a JSON URL, retrying a few times on transient TLS/socket read failures.
// Returns the parsed cJSON root (caller must cJSON_Delete) or nullptr.
cJSON* HttpGetJson(const std::string& url) {
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        bool transient = false;
        cJSON* root = HttpGetJsonOnce(url, &transient);
        if (root != nullptr || !transient) {
            return root;
        }
        if (attempt < kMaxAttempts) {
            ESP_LOGW(TAG, "GET attempt %d/%d failed transiently; retrying",
                     attempt, kMaxAttempts);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    return nullptr;
}

// Visit the homepage once so the API has a session cookie to echo back.
void EnsureCookie() {
    if (!g_cookie.empty()) {
        return;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    if (!http) {
        return;
    }
    http->SetTimeout(10000);
    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    if (http->Open("GET", std::string(kBase) + "/")) {
        http->GetStatusCode();      // block until headers are parsed...
        CaptureCookie(http.get());  // ...so the Set-Cookie header is present
    }
    http->Close();
}

std::string NowCtime() {
    return std::to_string(static_cast<long>(time(nullptr)));
}

}  // namespace

namespace zing {

bool SearchTopSong(const std::string& query, ZingSong& out) {
    EnsureCookie();
    const char* path = "/api/v2/search/multi";
    std::string ctime = NowCtime();
    std::string sig = HmacSha512Hex(kSecretKey,
        std::string(path) + Sha256Hex("ctime=" + ctime + "version=" + kVersion));
    if (sig.empty()) {
        return false;
    }
    std::string url = std::string(kBase) + path + "?q=" + UrlEncode(query) +
        "&ctime=" + ctime + "&version=" + kVersion + "&apiKey=" + kApiKey + "&sig=" + sig;

    cJSON* root = HttpGetJson(url);
    if (root == nullptr) {
        return false;
    }
    bool ok = false;
    cJSON* err = cJSON_GetObjectItem(root, "err");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsNumber(err) && err->valueint == 0 && cJSON_IsObject(data)) {
        cJSON* songs = cJSON_GetObjectItem(data, "songs");
        cJSON* first = nullptr;
        if (cJSON_IsArray(songs) && cJSON_GetArraySize(songs) > 0) {
            first = cJSON_GetArrayItem(songs, 0);
        } else {
            // Fallback: data.top is sometimes the best song match.
            cJSON* top = cJSON_GetObjectItem(data, "top");
            if (cJSON_IsObject(top) && cJSON_GetObjectItem(top, "encodeId") != nullptr) {
                first = top;
            }
        }
        if (first != nullptr) {
            cJSON* id = cJSON_GetObjectItem(first, "encodeId");
            cJSON* title = cJSON_GetObjectItem(first, "title");
            cJSON* artists = cJSON_GetObjectItem(first, "artistsNames");
            cJSON* thumb = cJSON_GetObjectItem(first, "thumbnailM");
            if (!cJSON_IsString(thumb)) {
                thumb = cJSON_GetObjectItem(first, "thumbnail");
            }
            if (cJSON_IsString(id)) {
                out.id = id->valuestring;
                out.title = cJSON_IsString(title) ? title->valuestring : "";
                out.artists = cJSON_IsString(artists) ? artists->valuestring : "";
                out.thumbnail = cJSON_IsString(thumb) ? thumb->valuestring : "";
                ok = true;
            }
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "search: no song for '%s' (err=%s)", query.c_str(),
                 cJSON_IsNumber(err) ? std::to_string(err->valueint).c_str() : "?");
    }
    cJSON_Delete(root);
    return ok;
}

bool GetStreamUrl(const std::string& song_id, std::string& url_out) {
    EnsureCookie();
    const char* path = "/api/v2/song/get/streaming";
    std::string ctime = NowCtime();
    std::string sig = HmacSha512Hex(kSecretKey,
        std::string(path) + Sha256Hex("ctime=" + ctime + "id=" + song_id + "version=" + kVersion));
    if (sig.empty()) {
        return false;
    }
    std::string url = std::string(kBase) + path + "?id=" + UrlEncode(song_id) +
        "&ctime=" + ctime + "&version=" + kVersion + "&apiKey=" + kApiKey + "&sig=" + sig;

    cJSON* root = HttpGetJson(url);
    if (root == nullptr) {
        return false;
    }
    bool ok = false;
    cJSON* err = cJSON_GetObjectItem(root, "err");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsNumber(err) && err->valueint == 0 && cJSON_IsObject(data)) {
        // Prefer 128 kbps (smaller, always free); fall back to 320.
        const char* keys[] = {"128", "320"};
        for (const char* k : keys) {
            cJSON* q = cJSON_GetObjectItem(data, k);
            if (cJSON_IsString(q) && std::string(q->valuestring).rfind("http", 0) == 0) {
                url_out = q->valuestring;
                ok = true;
                break;
            }
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "streaming: no free url (err=%s)",
                 cJSON_IsNumber(err) ? std::to_string(err->valueint).c_str() : "?");
    }
    cJSON_Delete(root);
    return ok;
}

}  // namespace zing
