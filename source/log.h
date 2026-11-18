#pragma once
// Registro de diagnóstico: se guarda en memoria (para mostrarlo en pantalla)
// y en sdmc:/switch/USBS2.log.
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

// Registro detallado (cada operación MTP y cada paso USB). Se activa manteniendo
// ZL al abrir la app; si no, solo se registran el arranque y los errores.
inline bool g_log_verbose = false;

inline Mutex &log_mutex() { static Mutex m = {}; return m; }
inline std::vector<std::string> &log_lines() { static std::vector<std::string> v; return v; }

inline void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_write(const char *fmt, ...) {
    char line[300];
    static u64 t0 = armTicksToNs(armGetSystemTick());
    u64 ms = (armTicksToNs(armGetSystemTick()) - t0) / 1000000ULL;
    int n = snprintf(line, sizeof(line), "%6llu.%03llu ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    va_list va;
    va_start(va, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, va);
    va_end(va);

    mutexLock(&log_mutex());
    auto &v = log_lines();
    v.push_back(line);
    if (v.size() > 200) v.erase(v.begin());
    if (FILE *f = fopen("sdmc:/switch/USBS2.log", "a")) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    mutexUnlock(&log_mutex());
}

#define log_debug(...) do { if (g_log_verbose) log_write(__VA_ARGS__); } while (0)

// Copia las últimas n líneas (para pintarlas en pantalla).
inline std::vector<std::string> log_tail(size_t n) {
    mutexLock(&log_mutex());
    auto &v = log_lines();
    std::vector<std::string> out(v.size() > n ? v.end() - n : v.begin(), v.end());
    mutexUnlock(&log_mutex());
    return out;
}
