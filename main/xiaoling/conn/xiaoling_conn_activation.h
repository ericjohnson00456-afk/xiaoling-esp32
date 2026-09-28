#ifndef XIAOLING_CONN_ACTIVATION_H
#define XIAOLING_CONN_ACTIVATION_H

#include <esp_err.h>

#include <string>

// 设备激活协议：检查当前 Product ID 是否接受设备，并在需要绑定时
// 使用服务端 challenge 完成激活。连接鉴权由 XiaolingConn 单独负责。
class XiaolingConnActivation final {
public:
    XiaolingConnActivation();

    bool Fetch();
    esp_err_t Submit() const;

    bool IsRequired() const { return !challenge_.empty(); }
    const std::string& message() const { return message_; }
    const std::string& code() const { return code_; }
    const std::string& qrcode() const { return qrcode_; }

private:
    std::string serial_number_;
    std::string message_;
    std::string code_;
    std::string challenge_;
    std::string qrcode_;
};

#endif  // XIAOLING_CONN_ACTIVATION_H
