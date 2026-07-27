#pragma once

// cpp_bus_driver 1.1.0 exposes the former I2S port type in its public headers.
// IDF 6 uses an integer port ID. Preserve the driver API without editing the
// managed component; both C and C++ translation units can include this shim.
#ifndef __ASSEMBLER__
typedef int i2s_port_t;
#endif
