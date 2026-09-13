#ifndef SERIAL_SHELL_H
#define SERIAL_SHELL_H

class SerialShell {
public:
    static SerialShell& GetInstance();

    SerialShell(const SerialShell&) = delete;
    SerialShell& operator=(const SerialShell&) = delete;

    bool Initialize();

private:
    SerialShell() = default;

    bool initialized_ = false;
};

#endif  // SERIAL_SHELL_H
