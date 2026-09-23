#pragma once
// ES: Utilidades de conversión ANSI (página de códigos del sistema) <-> UTF-8.
//     Las cadenas nativas del juego (p.ej. nombres) vienen en la codepage ANSI; la red y la UI usan UTF-8.
//     Usa MultiByteToWideChar / WideCharToMultiByte de Win32 (solo Windows).
// EN: ANSI (system codepage) <-> UTF-8 conversion utilities.
//     Native game strings (e.g. names) come in the ANSI codepage; network and UI use UTF-8.
//     Uses Win32 MultiByteToWideChar / WideCharToMultiByte (Windows only).

#ifdef _WIN32
#include <Windows.h>
#include <string>

namespace kmp {

// ES: Convierte bytes en la codepage ANSI del sistema a UTF-8 (pasando por UTF-16).
//     Devuelve la cadena UTF-8, o los bytes originales si falla.
// EN: Convert system ANSI codepage bytes to UTF-8.
//     Returns UTF-8 string, or original bytes on failure.
inline std::string AnsiToUtf8(const char* ansi, int len) {
    if (len <= 0) return {};
    // ES: ANSI -> UTF-16
    // EN: ANSI -> wide
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi, len, nullptr, 0);
    if (wlen <= 0) return std::string(ansi, len);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, len, wide.data(), wlen);
    // ES: UTF-16 -> UTF-8
    // EN: wide -> UTF-8
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return std::string(ansi, len);
    std::string utf8(u8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, utf8.data(), u8len, nullptr, nullptr);
    return utf8;
}

// ES: Convierte bytes UTF-8 a la codepage ANSI del sistema (pasando por UTF-16).
//     Devuelve la cadena ANSI, o los bytes originales si falla.
// EN: Convert UTF-8 bytes to system ANSI codepage.
//     Returns ANSI string, or original bytes on failure.
inline std::string Utf8ToAnsi(const char* utf8, int len) {
    if (len <= 0) return {};
    // ES: UTF-8 -> UTF-16
    // EN: UTF-8 -> wide
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    if (wlen <= 0) return std::string(utf8, len);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, wide.data(), wlen);
    // ES: UTF-16 -> ANSI
    // EN: wide -> ANSI
    int alen = WideCharToMultiByte(CP_ACP, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return std::string(utf8, len);
    std::string ansi(alen, '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.data(), wlen, ansi.data(), alen, nullptr, nullptr);
    return ansi;
}

// ES: Conversión ANSI->UTF-8 in situ sobre un búfer fijo.
//     Se usa dentro de bloques __try (SEH) donde std::string está prohibido (objetos con destructor).
//     Devuelve la nueva longitud, o la original si falla o no cabe. buf debe tener al menos bufSize bytes.
//     Límite interno de 256 caracteres por los búferes de pila.
// EN: In-place ANSI->UTF-8 conversion in a fixed buffer.
//     Used inside __try blocks where std::string is forbidden.
//     Returns new length, or original len on failure. buf must be at least bufSize bytes.
//     Internal limit of 256 characters due to the stack buffers.
inline int AnsiToUtf8InPlace(char* buf, int len, int bufSize) {
    if (len <= 0 || len >= bufSize - 1) return len;
    // ES: Si ya es ASCII puro no hace falta convertir.
    // EN: Check if already pure ASCII — no conversion needed
    bool allAscii = true;
    for (int i = 0; i < len; i++) {
        if (static_cast<unsigned char>(buf[i]) > 127) { allAscii = false; break; }
    }
    if (allAscii) return len;
    // ES: ANSI -> UTF-16 (búferes en pila por seguridad con SEH)
    // EN: ANSI -> wide (stack buffers for SEH safety)
    wchar_t wide[256];
    int wlen = MultiByteToWideChar(CP_ACP, 0, buf, len, wide, 256);
    if (wlen <= 0) return len;
    // ES: UTF-16 -> UTF-8
    // EN: wide -> UTF-8
    char utf8[256];
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, utf8, 256, nullptr, nullptr);
    if (u8len <= 0 || u8len >= bufSize) return len;
    memcpy(buf, utf8, u8len);
    buf[u8len] = '\0';
    return u8len;
}

} // namespace kmp
#endif
