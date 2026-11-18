#pragma once
#include <switch.h>
#include <string>
#include <vector>

namespace saves {
void init();
void exit();
void scan();                                   // vuelve a leer la lista de partidas
int count();
const std::string &name(int idx);              // "Juego - Usuario"
bool mount(int idx, std::string &dev_out);     // monta (o reutiliza) y devuelve "svN"
void commit(int idx);                          // guarda los cambios escritos
void unmount_all();
}
