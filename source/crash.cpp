// Si la app se cae (excepción de CPU), guarda en sdmc:/switch/USBS2-crash.txt
// dónde fue (dirección relativa a la app, registros y cadena de llamadas) y las
// últimas líneas del registro. Con USBS2.elf se traduce a archivo y línea.
#include <switch.h>
#include <cstdio>
#include "log.h"
#include "usb.h"

extern "C" {
extern char _start;
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    u64 base = (u64)&_start;
    FILE *f = fopen("sdmc:/switch/USBS2-crash.txt", "w");
    if (f) {
        fprintf(f, "USBS2 crash\nerror_desc 0x%X  esr 0x%X  far 0x%lX\n", ctx->error_desc, ctx->esr, ctx->far.x);
        fprintf(f, "base 0x%lX\npc  0x%lX (+0x%lX)\nlr  0x%lX (+0x%lX)\nsp  0x%lX\nfp  0x%lX\n",
                base, ctx->pc.x, ctx->pc.x - base, ctx->lr.x, ctx->lr.x - base, ctx->sp.x, ctx->fp.x);
        for (int i = 0; i < 29; i++) fprintf(f, "x%-2d 0x%016lX\n", i, ctx->cpu_gprs[i].x);
        fprintf(f, "llamadas:\n");
        u64 fp = ctx->fp.x;
        for (int i = 0; i < 24 && fp && !(fp & 7) && fp >= ctx->sp.x && fp - ctx->sp.x < 0x200000; i++) {
            u64 *fr = (u64 *)fp;
            u64 ret = fr[1];
            fprintf(f, "  0x%lX (+0x%lX)\n", ret, ret - base);
            if (fr[0] <= fp) break;
            fp = fr[0];
        }
        fprintf(f, "usb: %s / control: %s\nultimas lineas:\n", usb::where(), usb::where_ctrl());
        // sin bloquear el mutex: puede estar tomado por el hilo que cayó
        auto &v = log_lines();
        size_t n = v.size(), from = n > 30 ? n - 30 : 0;
        for (size_t i = from; i < n; i++) fprintf(f, "%s\n", v[i].c_str());
        fclose(f);
    }
    svcReturnFromException(0xF801);   // que el sistema cierre la app como siempre
}
}
