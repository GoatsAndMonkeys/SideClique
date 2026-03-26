#pragma once
// SCHacking — Fallout terminal hacking challenge (daily, Wordle-style)
//
// Same word for all clique members each day (seeded by date).
// 5-letter Fallout-themed words. 4 attempts. Shows letter matches.
// Based on the Fallout terminal hacking minigame.

#include <cstdint>
#include <cstring>
#include <cctype>

#define HACK_MAX_ATTEMPTS  4
#define HACK_WORD_LEN      5

// Tiny fallback word list (used only if wordle.bin not on external flash)
static const char HACK_WORDS_FALLBACK[][6] = {
    "vault", "power", "laser", "armor", "quest",
    "ghoul", "steel", "waste", "radio", "water",
};
static const uint16_t HACK_FALLBACK_COUNT = 10;

// Pick today's hacking word from external flash dictionary
// Uses a different seed offset than Wordle so the words differ each day
static bool hackPickWordExt(uint32_t day, char *word) {
#ifdef NRF52_SERIES
    // Try external flash dictionary (same file as Wordle)
    if (wordleExtDictAvailable()) {
        // Offset the day by a large prime so hacking word != wordle word
        return wordlePickWordExt(day + 7919, word);
    }
#endif
    // Fallback to embedded list
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < 4; i++) { h ^= ((day + 7919) >> (i * 8)) & 0xFF; h *= 0x01000193; }
    const char *w = HACK_WORDS_FALLBACK[h % HACK_FALLBACK_COUNT];
    strncpy(word, w, 5);
    word[5] = '\0';
    return true;
}

// Validate guess against external dictionary
static bool hackValidateWord(const char *guess) {
#ifdef NRF52_SERIES
    if (wordleExtDictAvailable())
        return wordleIsValidExt(guess);
#endif
    return wordleIsValid(guess); // accept any 5-letter alpha on nRF52 without dict
}

// Count exact matches (right letter, right position)
static uint8_t hackExactMatches(const char *guess, const char *target) {
    uint8_t count = 0;
    for (int i = 0; i < HACK_WORD_LEN; i++) {
        if (tolower((unsigned char)guess[i]) == tolower((unsigned char)target[i]))
            count++;
    }
    return count;
}

// Build feedback string: shows the word + match count (Fallout style)
// "VAULT - 3/5 match"
static void hackFeedback(const char *guess, const char *target, char *fb, size_t fbLen) {
    uint8_t exact = hackExactMatches(guess, target);

    // Also show per-letter: * = exact, . = wrong
    char letters[6];
    for (int i = 0; i < HACK_WORD_LEN; i++) {
        char g = tolower((unsigned char)guess[i]);
        char t = tolower((unsigned char)target[i]);
        letters[i] = (g == t) ? '*' : '.';
    }
    letters[5] = '\0';

    char upper[6];
    for (int i = 0; i < HACK_WORD_LEN; i++)
        upper[i] = toupper((unsigned char)guess[i]);
    upper[5] = '\0';

    snprintf(fb, fbLen, "%s [%s] %u/%u", upper, letters, exact, HACK_WORD_LEN);
}

// Validate guess is 5 alpha chars
static bool hackValidGuess(const char *text, char *out) {
    int len = 0;
    for (int i = 0; text[i] && len < HACK_WORD_LEN; i++) {
        if (isalpha((unsigned char)text[i]))
            out[len++] = tolower((unsigned char)text[i]);
    }
    out[len] = '\0';
    return len == HACK_WORD_LEN;
}
