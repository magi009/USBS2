#pragma once
// Registro de diagnóstico: se guarda en memoria (para mostrarlo en pantalla)
// y se intenta escribir en sdmc:/USBS2.log y sdmc:/switch/USBS2.log.
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

inline Mutex &log_mutex() { static Mutex m = {}; return m; }
inline std::vector<std::string> &log_lines() { static std::vector<std::string> v; return v; }

inline void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_write(const char *fmt, ...) {
    char line[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);

    mutexLock(&log_mutex());
    auto &v = log_lines();
    v.push_back(line);
    if (v.size() > 200) v.erase(v.begin());
    for (const char *path : {"sdmc:/USBS2.log", "sdmc:/switch/USBS2.log"}) {
        if (FILE *f = fopen(path, "a")) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
    mutexUnlock(&log_mutex());
}

// Copia las últimas n líneas (para pintarlas en pantalla).
inline std::vector<std::string> log_tail(size_t n) {
    mutexLock(&log_mutex());
    auto &v = log_lines();
    std::vector<std::string> out(v.size() > n ? v.end() - n : v.begin(), v.end());
    mutexUnlock(&log_mutex());
    return out;
}
