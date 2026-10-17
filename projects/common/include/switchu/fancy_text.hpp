#pragma once

// Discord "fonts": the styled Unicode letters people put in names and
// messages (bold, script, fraktur, double-struck, fullwidth, circled,
// squared, small caps, superscript, subscript...) as the plain letters they
// stand for, for when no font on the console has the real character. And the
// marks better left out on screen: accents typed apart from their letter,
// zalgo, strike-through and underline marks, zero-width characters.
// Shared by HOME's Discord panel and the Discord overlay.

#include <cstdint>

namespace switchu {

inline std::uint32_t plainLetter(std::uint32_t cp) {
    // Mathematical alphanumerics: 13 Latin styles of A-Z a-z (their gaps are
    // the Letterlike Symbols further down), 5 Greek styles, 5 sets of digits.
    if (cp >= 0x1D400 && cp <= 0x1D6A3) {
        const std::uint32_t o = (cp - 0x1D400) % 52;
        return o < 26 ? 'A' + o : 'a' + (o - 26);
    }
    if (cp == 0x1D6A4) return 'i';
    if (cp == 0x1D6A5) return 'j';
    if (cp >= 0x1D6A8 && cp <= 0x1D7C9) {
        const std::uint32_t o = (cp - 0x1D6A8) % 58;
        if (o < 17) return 0x391 + o;            // Alpha..Rho
        if (o == 17) return 0x398;               // Theta symbol
        if (o < 25) return 0x3A3 + (o - 18);     // Sigma..Omega
        if (o == 25) return 0x2207;              // nabla
        if (o < 51) return 0x3B1 + (o - 26);     // alpha..omega
        static const std::uint16_t kTail[] = {0x2202, 0x3B5, 0x3B8, 0x3BA, 0x3C6, 0x3C1, 0x3C0};
        return kTail[o - 51];
    }
    if (cp >= 0x1D7CE && cp <= 0x1D7FF) return '0' + (cp - 0x1D7CE) % 10;
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFF01 + 0x21;      // fullwidth
    if (cp >= 0x24B6 && cp <= 0x24CF) return 'A' + (cp - 0x24B6);     // circled
    if (cp >= 0x24D0 && cp <= 0x24E9) return 'a' + (cp - 0x24D0);
    if (cp >= 0x2460 && cp <= 0x2468) return '1' + (cp - 0x2460);     // circled 1-9
    if (cp >= 0x2776 && cp <= 0x277E) return '1' + (cp - 0x2776);     // negative circled 1-9
    if (cp == 0x24EA || cp == 0x24FF) return '0';
    if (cp >= 0x249C && cp <= 0x24B5) return 'a' + (cp - 0x249C);     // parenthesized
    if (cp >= 0x1F110 && cp <= 0x1F129) return 'A' + (cp - 0x1F110);
    if (cp >= 0x1F130 && cp <= 0x1F149) return 'A' + (cp - 0x1F130); // squared
    if (cp >= 0x1F150 && cp <= 0x1F169) return 'A' + (cp - 0x1F150); // negative circled
    if (cp >= 0x1F170 && cp <= 0x1F189) return 'A' + (cp - 0x1F170); // negative squared
    if (cp == 0x2070 || (cp >= 0x2074 && cp <= 0x2079)) return '0' + (cp - 0x2070); // superscript digits
    if (cp >= 0x2080 && cp <= 0x2089) return '0' + (cp - 0x2080);     // subscript digits
    switch (cp) {
        case 0xB9: return '1';
        case 0xB2: return '2';
        case 0xB3: return '3';
        // Letterlike symbols: the script, fraktur and double-struck letters
        // that the mathematical sets leave out.
        case 0x212C: return 'B'; case 0x2130: return 'E'; case 0x2131: return 'F'; case 0x210B: return 'H';
        case 0x2110: return 'I'; case 0x2112: return 'L'; case 0x2133: return 'M'; case 0x211B: return 'R';
        case 0x212F: return 'e'; case 0x210A: return 'g'; case 0x2134: return 'o'; case 0x212D: return 'C';
        case 0x210C: return 'H'; case 0x2111: return 'I'; case 0x211C: return 'R'; case 0x2128: return 'Z';
        case 0x2102: return 'C'; case 0x210D: return 'H'; case 0x2115: return 'N'; case 0x2119: return 'P';
        case 0x211A: return 'Q'; case 0x211D: return 'R'; case 0x2124: return 'Z'; case 0x210E: return 'h';
        // Small capitals.
        case 0x1D00: return 'a'; case 0x0299: return 'b'; case 0x1D04: return 'c'; case 0x1D05: return 'd';
        case 0x1D07: return 'e'; case 0xA730: return 'f'; case 0x0262: return 'g'; case 0x029C: return 'h';
        case 0x026A: return 'i'; case 0x1D0A: return 'j'; case 0x1D0B: return 'k'; case 0x029F: return 'l';
        case 0x1D0D: return 'm'; case 0x0274: return 'n'; case 0x1D0F: return 'o'; case 0x1D18: return 'p';
        case 0x01EB: return 'q'; case 0x0280: return 'r'; case 0xA731: return 's'; case 0x1D1B: return 't';
        case 0x1D1C: return 'u'; case 0x1D20: return 'v'; case 0x1D21: return 'w'; case 0x028F: return 'y';
        case 0x1D22: return 'z';
        // Superscript letters.
        case 0x1D43: return 'a'; case 0x1D47: return 'b'; case 0x1D9C: return 'c'; case 0x1D48: return 'd';
        case 0x1D49: return 'e'; case 0x1DA0: return 'f'; case 0x1D4D: return 'g'; case 0x02B0: return 'h';
        case 0x2071: return 'i'; case 0x02B2: return 'j'; case 0x1D4F: return 'k'; case 0x02E1: return 'l';
        case 0x1D50: return 'm'; case 0x207F: return 'n'; case 0x1D52: return 'o'; case 0x1D56: return 'p';
        case 0x02B3: return 'r'; case 0x02E2: return 's'; case 0x1D57: return 't'; case 0x1D58: return 'u';
        case 0x1D5B: return 'v'; case 0x02B7: return 'w'; case 0x02E3: return 'x'; case 0x02B8: return 'y';
        case 0x1DBB: return 'z';
        // Subscript letters.
        case 0x2090: return 'a'; case 0x2091: return 'e'; case 0x2092: return 'o'; case 0x2093: return 'x';
        case 0x2095: return 'h'; case 0x2096: return 'k'; case 0x2097: return 'l'; case 0x2098: return 'm';
        case 0x2099: return 'n'; case 0x209A: return 'p'; case 0x209B: return 's'; case 0x209C: return 't';
        case 0x1D62: return 'i'; case 0x2C7C: return 'j'; case 0x1D63: return 'r'; case 0x1D64: return 'u';
        case 0x1D65: return 'v';
        default: return 0;
    }
}

// Marks drawn onto the letter before them, and characters with nothing to
// draw: left out on screen (fonts here can't stack marks, they'd show apart).
inline bool dropFromScreen(std::uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x1AB0 && cp <= 0x1AFF)
        || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F)
        || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064)
        || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0x00AD;
}

} // namespace switchu
