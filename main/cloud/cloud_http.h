#ifndef CLOUD_HTTP_H
#define CLOUD_HTTP_H

#include <string>
#include <utility>
#include <vector>

using CloudHttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct CloudHttpResponse {
    int status_code = 0;
    std::string body;
};

// 对 managed_components HTTP 实现的薄封装，只负责请求传输。
class CloudHttp {
public:
    bool Get(const std::string& url, CloudHttpResponse& response,
             const CloudHttpHeaders& headers = {}, int timeout_ms = 0) const;
    bool PostJson(const std::string& url, const std::string& body,
                  CloudHttpResponse& response, const CloudHttpHeaders& headers = {},
                  int timeout_ms = 0) const;

private:
    bool Request(const std::string& method, const std::string& url,
                 const std::string& body, CloudHttpResponse& response,
                 const CloudHttpHeaders& headers, int timeout_ms) const;
};

#endif // CLOUD_HTTP_H
