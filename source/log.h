#pragma once
// Registro en sdmc:/USBS2.log. Abre y cierra el archivo en cada línea para
// que lo escrito sobreviva aunque la app se cuelgue justo después.
#include <cstdio>
#include <cstdarg>

inline void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_write(const char *fmt, ...) {
    FILE *f = fopen("sdmc:/USBS2.log", "a");
    if (!f) return;
    va_list va;
    va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fputc('\n', f);
    fclose(f);
}
