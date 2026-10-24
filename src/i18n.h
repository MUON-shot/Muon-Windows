#pragma once
// Two UI languages. Strings are written inline as T("русский", "English").
extern int g_lang;   // 0 = Russian, 1 = English

inline const char* T(const char* ru, const char* en) { return g_lang == 1 ? en : ru; }
inline const wchar_t* TW(const wchar_t* ru, const wchar_t* en) { return g_lang == 1 ? en : ru; }
