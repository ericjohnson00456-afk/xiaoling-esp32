#ifndef XIAOLING_AUTH_H
#define XIAOLING_AUTH_H

#include <mutex>
#include <string>

// lschat 鉴权返回的短期访问凭据。
struct XiaolingAuthConfig {
    std::string access_token;

    bool IsValid() const;
};

// 负责小聆 lschat token 鉴权和进程内凭据管理。
class XiaolingAuth {
public:
    static XiaolingAuth& GetInstance();

    XiaolingAuthConfig Load() const;
    bool Save(const XiaolingAuthConfig& config) const;
    void Clear() const;

    bool Authenticate(const std::string& url,
                      const std::string& product_id,
                      const std::string& device_id,
                      const std::string& secret_id,
                      const std::string& extra_code = "") const;

    static std::string NormalizeBearerToken(const std::string& token);

private:
    XiaolingAuth() = default;
    XiaolingAuth(const XiaolingAuth&) = delete;
    XiaolingAuth& operator=(const XiaolingAuth&) = delete;

    mutable std::mutex mutex_;
    mutable XiaolingAuthConfig config_;
};

#endif // XIAOLING_AUTH_H
