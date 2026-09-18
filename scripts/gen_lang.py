#!/usr/bin/env python3
import argparse
import json
import os


HEADER_TEMPLATE = """// Auto-generated language config. Do not edit manually.
// Primary language: {lang_code}; runtime fallback: en-US
#pragma once

#ifndef {lang_code_for_font}
#define {lang_code_for_font}
#endif

#include <cstring>

namespace Lang {{

enum class Language {{
    Primary,
    English,
}};

namespace PrimaryStrings {{
{primary_strings}
}}  // namespace PrimaryStrings

namespace EnglishStrings {{
{english_strings}
}}  // namespace EnglishStrings

inline Language current_language = Language::Primary;
inline const char* CODE = "{lang_code}";

// Board code and UI code share this runtime-selected string table.
namespace Strings {{
{active_strings}
}}  // namespace Strings

inline Language GetLanguage() {{
    return current_language;
}}

inline const char* GetLanguageCode() {{
    return CODE;
}}

inline void SetLanguage(Language language) {{
    current_language = language;
    CODE = language == Language::English ? "en-US" : "{lang_code}";
{assign_strings}
}}

inline bool SetLanguageByCode(const char* code) {{
    if (code == nullptr) {{
        return false;
    }}
    if (std::strcmp(code, "en-US") == 0) {{
        SetLanguage(Language::English);
        return true;
    }}
    if (std::strcmp(code, "{lang_code}") == 0) {{
        SetLanguage(Language::Primary);
        return true;
    }}
    return false;
}}

inline Language ToggleLanguage() {{
    const auto next = current_language == Language::Primary
                          ? Language::English
                          : Language::Primary;
    SetLanguage(next);
    return next;
}}

namespace Sounds {{
}}  // namespace Sounds

}}  // namespace Lang
"""


def load_language(assets_dir, lang_code):
    path = os.path.join(assets_dir, "locales", lang_code, "language.json")
    if not os.path.exists(path):
        raise FileNotFoundError(f"Language file not found: {path}")
    with open(path, "r", encoding="utf-8") as file:
        data = json.load(file)
    if "language" not in data or "strings" not in data:
        raise ValueError(f"Invalid language JSON structure: {path}")
    return data["strings"]


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def generate_header(lang_code, output_path):
    main_dir = os.path.dirname(output_path)
    while os.path.basename(main_dir) != "main" and os.path.dirname(main_dir) != main_dir:
        main_dir = os.path.dirname(main_dir)
    assets_dir = os.path.join(main_dir, "resource", "assets")

    english_source = load_language(assets_dir, "en-US")
    primary_source = load_language(assets_dir, lang_code)
    keys = sorted(set(english_source) | set(primary_source))
    primary = {key: primary_source.get(key, english_source[key]) for key in keys}
    english = {key: english_source.get(key, primary[key]) for key in keys}

    primary_lines = [
        f"inline constexpr const char* {key.upper()} = {cpp_string(primary[key])};"
        for key in keys
    ]
    english_lines = [
        f"inline constexpr const char* {key.upper()} = {cpp_string(english[key])};"
        for key in keys
    ]
    active_lines = [
        f"inline const char* {key.upper()} = PrimaryStrings::{key.upper()};"
        for key in keys
    ]
    assign_lines = [
        f"    Strings::{key.upper()} = language == Language::English "
        f"? EnglishStrings::{key.upper()} : PrimaryStrings::{key.upper()};"
        for key in keys
    ]

    content = HEADER_TEMPLATE.format(
        lang_code=lang_code,
        lang_code_for_font=lang_code.replace("-", "_").lower(),
        primary_strings="\n".join(primary_lines),
        english_strings="\n".join(english_lines),
        active_strings="\n".join(active_lines),
        assign_strings="\n".join(assign_lines),
    )
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as file:
        file.write(content)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate a runtime-switchable language header")
    parser.add_argument("--language", required=True, help="Primary language code")
    parser.add_argument("--output", required=True, help="Output header path")
    args = parser.parse_args()
    generate_header(args.language, args.output)
    print(f"Generated runtime language config: {args.output}")
