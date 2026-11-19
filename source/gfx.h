#pragma once
// Dibujo simple por software sobre el framebuffer (1280x720, RGB565):
// rectángulos redondeados, degradados y texto con la fuente Inter (data/font.bin).
#include <switch.h>

namespace gfx {
constexpr int W = 1280, H = 720;

enum Font { Small = 0, Body = 1, Big = 2, Title = 3 };

bool init();
void exit();

void begin();   // empieza un fotograma (fondo azul ya pintado)
void end();     // lo muestra

void rect(int x, int y, int w, int h, u32 rgb, u8 alpha = 255);
void round_rect(int x, int y, int w, int h, int r, u32 rgb, u8 alpha = 255);
// Degradado vertical dentro de un rectángulo redondeado.
void round_rect_v(int x, int y, int w, int h, int r, u32 top, u32 bottom);

// y = parte de arriba de la línea. max_w > 0 recorta con "…". Devuelve el ancho.
int text(Font f, int x, int y, u32 rgb, const char *utf8, int max_w = 0);
int text_w(Font f, const char *utf8);
int line_h(Font f);
}
