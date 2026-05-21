#include "rest.h"
#include <cstring>
#include <whb/log.h>

static const char *BASE_URL = "https://discord.com/api/v10";

namespace Discord {

RestClient::RestClient(const std::string &token) : token_(token) {
    curl_ = curl_easy_init();
}

RestClient::~RestClient() {
    if (curl_) curl_easy_cleanup(curl_);
}

size_t RestClient::write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<std::string *>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string RestClient::request(const std::string &method,
                                const std::string &endpoint,
                                const std::string &body) {
    if (!curl_) return {};

    std::string url = std::string(BASE_URL) + endpoint;
    std::string response;
    std::string auth_header = "Authorization: " + token_;
    std::string content_type = "Content-Type: application/json";

    curl_easy_reset(curl_);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, content_type.c_str());
    headers = curl_slist_append(headers, "User-Agent: DiscordBot (WiiU, 1.0.0)");

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);

    if (method == "POST") {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "PATCH") {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode res = curl_easy_perform(curl_);
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &last_http_code_);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        WHBLogPrintf("REST %s %s failed: %s", method.c_str(), endpoint.c_str(), curl_easy_strerror(res));
        return {};
    }

    return response;
}

std::string RestClient::get(const std::string &endpoint) {
    return request("GET", endpoint, {});
}

std::string RestClient::post(const std::string &endpoint, const std::string &body) {
    return request("POST", endpoint, body);
}

std::string RestClient::patch(const std::string &endpoint, const std::string &body) {
    return request("PATCH", endpoint, body);
}

std::string RestClient::del(const std::string &endpoint) {
    return request("DELETE", endpoint, {});
}

} // namespace Discord
