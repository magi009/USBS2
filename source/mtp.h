#pragma once
#include <switch.h>
#include <atomic>
#include <cstddef>

namespace mtp {
Result start(std::atomic<bool> *quit);   // arranca USB + hilo MTP
void stop();                              // espera al hilo y libera todo
void status(char *out, size_t size, u64 *bytes);
}
