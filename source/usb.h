#pragma once
#include <switch.h>
#include <atomic>

namespace usb {
Result init(std::atomic<bool> *quit_flag);
void exit();
bool wait_ready();                          // espera a que el PC configure el dispositivo
u32 max_packet();                           // tamaño máximo de paquete bulk actual
bool read(void *buf, u32 size, u32 *out);   // buf alineado a 0x1000
bool write(const void *buf, u32 size);      // buf alineado a 0x1000
}
