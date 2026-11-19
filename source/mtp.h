#pragma once
#include <switch.h>
#include <atomic>
#include <cstddef>

namespace mtp {
Result start(std::atomic<bool> *quit);   // arranca USB + hilo MTP
bool stop();                              // espera al hilo y libera todo
void status(char *out, size_t size, u64 *bytes);
struct Progress { int dir; u64 done, total; u32 files; char name[96]; }; // dir: 0 nada, 1 PC->consola, 2 consola->PC
void progress(Progress *out);
const char *link_name();   // tipo de conexión USB negociada con el PC
}
