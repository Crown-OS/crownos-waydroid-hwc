/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "input/keyboard_layout.h"

#include <utility>

namespace input {
namespace {

/// xkb layout descriptions (xkeyboard-config) by prefix, most specific first.
constexpr std::pair<std::string_view, std::string_view> layouts[] = {
    {"English (US, intl", "english_us_intl"},
    {"English (intl", "english_us_intl"},
    {"English (Colemak", "english_us_colemak"},
    {"English (Dvorak", "english_us_dvorak"},
    {"English (Workman", "english_us_workman"},
    {"English (UK", "english_uk"},
    {"English (US", "english_us"},
    {"English", "english_us"},
    {"German (Switzerland", "swiss_german"},
    {"German", "german"},
    {"French (Switzerland", "swiss_french"},
    {"French (Canada", "french_ca"},
    {"Canadian (CSA", "french_ca"},
    {"French", "french"},
    {"Russian (Macintosh", "russian_mac"},
    {"Russian", "russian"},
    {"Spanish (Latin American", "spanish_latin"},
    {"Spanish", "spanish"},
    {"Belgian", "belgian"},
    {"Bulgarian (traditional phonetic", "bulgarian_phonetic"},
    {"Bulgarian (new phonetic", "bulgarian_phonetic"},
    {"Bulgarian", "bulgarian"},
    {"Italian", "italian"},
    {"Danish", "danish"},
    {"Norwegian", "norwegian"},
    {"Swedish", "swedish"},
    {"Finnish", "finnish"},
    {"Croatian", "croatian"},
    {"Czech (QWERTY", "czech_qwerty"},
    {"Czech", "czech"},
    {"Estonian", "estonian"},
    {"Hungarian", "hungarian"},
    {"Icelandic", "icelandic"},
    {"Portuguese (Brazil", "brazilian"},
    {"Portuguese", "portuguese"},
    {"Slovak", "slovak"},
    {"Slovenian", "slovenian"},
    {"Turkish (F", "turkish_f"},
    {"Turkish", "turkish"},
    {"Ukrainian", "ukrainian"},
    {"Arabic", "arabic"},
    {"Greek", "greek"},
    {"Hebrew", "hebrew"},
    {"Lithuanian", "lithuanian"},
    {"Latvian", "latvian"},
    {"Persian", "persian"},
    {"Azerbaijani", "azerbaijani"},
    {"Polish", "polish"},
    {"Belarusian", "belarusian"},
    {"Mongolian", "mongolian"},
    {"Georgian", "georgian"},
    {"Serbian (Latin", "croatian"},
    {"Bosnian", "croatian"},
    {"Irish", "english_uk"},
};

bool matches(std::string_view name, std::string_view prefix) {
    if (name.substr(0, prefix.size()) != prefix) {
        return false;
    }
    if (name.size() == prefix.size()) {
        return true;
    }
    const bool names_a_variant = prefix.find('(') != std::string_view::npos;
    return names_a_variant || name[prefix.size()] == ' ';
}

} // namespace

std::optional<std::string_view> android_keyboard_layout(std::string_view xkb_layout_name) {
    for (const auto &[prefix, layout] : layouts) {
        if (matches(xkb_layout_name, prefix)) {
            return layout;
        }
    }
    return std::nullopt;
}

} // namespace input
