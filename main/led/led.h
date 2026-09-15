#ifndef _LED_H_
#define _LED_H_

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;
    // Optional manual controls used by the MCP device tools. Board LEDs that
    // do not support these operations keep the default no-op behavior.
    virtual void TurnOn() {}
    virtual void TurnOff() {}
    virtual void Blink(int interval_ms) { (void)interval_ms; }
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
