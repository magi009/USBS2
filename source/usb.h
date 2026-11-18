#pragma once
#include <switch.h>
#include <atomic>

namespace usb {
enum class Status { Ok, Timeout, Error };

Result init(std::atomic<bool> *quit_flag);
void exit();
bool wait_ready();      // espera a que el PC configure el dispositivo
u32 max_packet();       // tamaño máximo de paquete bulk (se mide al conectar)

// Transferencias. Los buffers deben estar alineados a 0x1000.
// timeout_ms = 0: esperar sin límite (solo se corta si se desconecta el cable).
// Timeout: el PC dejó de mandar/recibir (p. ej. canceló la copia).
Status read(void *buf, u32 size, u32 *out, u32 timeout_ms = 0);
Status write(const void *buf, u32 size, u32 timeout_ms = 0);

// Versión en dos pasos para solapar USB con lectura/escritura de la micro.
bool begin_read(void *buf, u32 size);
Status end_read(u32 *out, u32 timeout_ms = 0);
bool begin_write(const void *buf, u32 size);
Status end_write(u32 timeout_ms = 0);
void cancel_all();
u32 setup_count();
bool cancel_requested();   // el PC canceló la operación en curso (Cancel Request)
void clear_cancel();   // peticiones recibidas por el canal de control (diagnóstico)
}
