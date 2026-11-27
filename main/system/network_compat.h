#pragma once

#include <string>

// The classic ESP32 retains the non-DMA modem component. Keep its integer
// results and the newer component's expected results behind one boundary.
inline int NetworkValue(int value) { return value; }

template <typename Result>
int NetworkValue(const Result& result) { return result.value_or(-1); }

inline int NetworkErrorCode(int error) { return error; }
inline std::string NetworkErrorText(int error) { return std::to_string(error); }

template <typename Error>
int NetworkErrorCode(const Error& error) { return error.native != 0 ? error.native : -1; }

template <typename Error>
std::string NetworkErrorText(const Error& error) { return error.ToString(); }
