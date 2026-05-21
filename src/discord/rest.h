#pragma once
#include <string>
#include <functional>
#include <curl/curl.h>

namespace Discord {

// Thin wrapper around libcurl for Discord's REST API.
// All calls are synchronous. Use from a worker thread if needed.
class RestClient {
public:
    explicit RestClient(const std::string &token);
    ~RestClient();

    // Returns response body; empty string on failure.
    std::string get(const std::string &endpoint);
    std::string post(const std::string &endpoint, const std::string &json_body);
    std::string patch(const std::string &endpoint, const std::string &json_body);
    std::string del(const std::string &endpoint);

    int last_http_code() const { return last_http_code_; }

private:
    static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
    std::string request(const std::string &method,
                        const std::string &endpoint,
                        const std::string &body);

    std::string token_;
    int         last_http_code_ = 0;
    CURL       *curl_ = nullptr;
};

} // namespace Discord
