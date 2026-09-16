#ifndef MICRO_WAKE_WORD_MODEL_WAKE_WORD_H
#define MICRO_WAKE_WORD_MODEL_WAKE_WORD_H

#if CONFIG_MICRO_WAKE_WORD_MODEL_ALEXA
inline constexpr char kMicroWakeWordModelWakeWord[] = "Alexa";
#elif CONFIG_MICRO_WAKE_WORD_MODEL_HEY_JARVIS
inline constexpr char kMicroWakeWordModelWakeWord[] = "Hey Jarvis";
#elif CONFIG_MICRO_WAKE_WORD_MODEL_HEY_MYCROFT
inline constexpr char kMicroWakeWordModelWakeWord[] = "Hey Mycroft";
#elif CONFIG_MICRO_WAKE_WORD_MODEL_OKAY_NABU
inline constexpr char kMicroWakeWordModelWakeWord[] = "Okay Nabu";
#elif CONFIG_MICRO_WAKE_WORD_MODEL_XIAOLINGXIAOLING
// Keep non-ASCII model labels out of Windows compiler command-line definitions.
inline constexpr char kMicroWakeWordModelWakeWord[] =
    "\xE5\xB0\x8F\xE8\x81\x86\xE5\xB0\x8F\xE8\x81\x86";
static_assert(sizeof(kMicroWakeWordModelWakeWord) == 13,
              "The Xiaoling wake-word label must be 12-byte UTF-8");
#elif CONFIG_MICRO_WAKE_WORD_MODEL_HI_LINA
inline constexpr char kMicroWakeWordModelWakeWord[] = "Hi Lina";
#else
#error "No MicroWakeWord model selected"
#endif

#endif  // MICRO_WAKE_WORD_MODEL_WAKE_WORD_H
