#pragma once
// ReSaverHelper builds ReSaver Plus's AI chat (src/gui/Ai.cpp) for the F1 page "Ask the AI" (version 23). The helper
// has no translations: this stands in for ReSaver Plus's core/Lang.h and gives every text back in English.
inline const char* tr(const char* a_en) { return a_en; }
inline const char* trId(const char* a_en) { return a_en; }
inline const char* trc(const char*, const char* a_en) { return a_en; }
#define N_(a_text) a_text
