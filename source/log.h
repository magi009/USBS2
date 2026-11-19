#pragma once
// Registro de diagnóstico. Siempre se guarda en memoria (últimas 200 líneas);
// solo se escribe en sdmc:/switch/USBS2.log cuando el usuario lo activa con Y.
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

// Registro activado (botón Y): escribe el archivo y anota cada operación MTP/USB.
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
    if (g_log_verbose)
        if (FILE *f = fopen("sdmc:/switch/USBS2.log", "a")) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    mutexUnlock(&log_mutex());
}

// Activa o desactiva el registro. La primera vez en la sesión guarda el log
// anterior como USBS2-anterior.log y vuelca lo que ya había en memoria.
inline void log_enable(bool on) {
    static bool started = false;
    mutexLock(&log_mutex());
    if (on && !started) {
        started = true;
        remove("sdmc:/switch/USBS2-anterior.log");
        rename("sdmc:/switch/USBS2.log", "sdmc:/switch/USBS2-anterior.log");
        if (FILE *f = fopen("sdmc:/switch/USBS2.log", "w")) {
            for (auto &l : log_lines()) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
    }
    g_log_verbose = on;
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
