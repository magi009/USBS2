// mtp.cpp - Respondedor MTP mínimo: el PC ve la consola como un dispositivo
// con 3 unidades: Tarjeta SD (sdmc:/), Homebrew (sdmc:/switch) y Partidas guardadas.
#include "mtp.h"
#include "usb.h"
#include "saves.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <malloc.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>

namespace mtp {

// ---------------- constantes MTP ----------------
enum : u16 {
    CT_COMMAND = 1, CT_DATA = 2, CT_RESPONSE = 3,

    OP_GetDeviceInfo = 0x1001, OP_OpenSession = 0x1002, OP_CloseSession = 0x1003,
    OP_GetStorageIDs = 0x1004, OP_GetStorageInfo = 0x1005, OP_GetObjectHandles = 0x1007,
    OP_GetObjectInfo = 0x1008, OP_GetObject = 0x1009, OP_DeleteObject = 0x100B,
    OP_SendObjectInfo = 0x100C, OP_SendObject = 0x100D, OP_GetDevicePropDesc = 0x1014,
    OP_GetDevicePropValue = 0x1015, OP_MoveObject = 0x1019, OP_GetPartialObject = 0x101B,
    OP_GetObjectPropsSupported = 0x9801, OP_GetObjectPropDesc = 0x9802,
    OP_GetObjectPropValue = 0x9803, OP_SetObjectPropValue = 0x9804,
    OP_GetObjectPropList = 0x9805, OP_SendObjectPropList = 0x9808, OP_GetObjectReferences = 0x9810,

    RC_OK = 0x2001, RC_GeneralError = 0x2002, RC_SessionNotOpen = 0x2003,
    RC_OperationNotSupported = 0x2005, RC_InvalidStorageID = 0x2008,
    RC_InvalidObjectHandle = 0x2009, RC_DevicePropNotSupported = 0x200A,
    RC_StoreFull = 0x200C, RC_AccessDenied = 0x200F, RC_NoValidObjectInfo = 0x2015,
    RC_InvalidParentObject = 0x201A, RC_SessionAlreadyOpen = 0x201E,
    RC_InvalidObjectPropCode = 0xA801, RC_SpecificationByGroupUnsupported = 0xA807,
    RC_SpecificationByDepthUnsupported = 0xA808,

    FMT_Undefined = 0x3000, FMT_Association = 0x3001,

    PROP_StorageID = 0xDC01, PROP_ObjectFormat = 0xDC02, PROP_ProtectionStatus = 0xDC03,
    PROP_ObjectSize = 0xDC04, PROP_ObjectFileName = 0xDC07, PROP_DateModified = 0xDC09,
    PROP_ParentObject = 0xDC0B, PROP_PersistentUID = 0xDC41, PROP_Name = 0xDC44,

    DPROP_FriendlyName = 0xD402,

    DT_UINT16 = 0x0004, DT_UINT32 = 0x0006, DT_UINT64 = 0x0008, DT_UINT128 = 0x000A, DT_STR = 0xFFFF,
};

static constexpr u32 STOR_SD = 0x00010001, STOR_HB = 0x00020001, STOR_SV = 0x00030001;
static constexpr u32 ROOT = 0xFFFFFFFF;
static constexpr u32 BUF_SIZE = 4 * 1024 * 1024;

// ---------------- estado ----------------
static std::atomic<bool> *g_quit;
static u8 *g_buf;   // datos (alineado 0x1000)
static u8 *g_buf2;  // segundo buffer de datos: USB y micro trabajan a la vez
static u8 *g_cmd;   // comandos y respuestas (alineado 0x1000)
static Mutex g_status_mtx;
static char g_status[128] = "Esperando cable USB...";
static std::atomic<u64> g_bytes{0};
static bool g_session = false;

// Si el PC deja de mandar/recibir datos A MITAD de una transferencia durante
// este tiempo, se da por cancelada y se vuelve a esperar comandos. (Al empezar
// un archivo no hay límite: Windows puede tardar en tener los datos listos.)
static constexpr u32 IO_TIMEOUT_MS = 30000;
static bool g_aborted = false;          // la operación en curso fue cancelada por el PC
static u8 g_pending_cmd[32];            // comando que llegó en mitad de una fase de datos
static u32 g_pending_len = 0;
// Canal PC -> consola: SIEMPRE hay varias lecturas publicadas (cola de 3). Mientras
// se procesa un bloque, las otras siguen recibiendo: nunca hay un instante en que
// el PC mande datos sin nadie escuchando (en Switch 2 eso hace que Windows cancele).
static constexpr u32 RX_SIZE = 2 * 1024 * 1024;
static constexpr int RX_N = 3;
static u8 *g_rx[RX_N];
static bool g_rx_running = false;
static int g_rx_held = -1;     // buffer que se está procesando (se vuelve a publicar en rx_next)

static void rx_reset() {
    usb::rx_stop();
    g_rx_running = false;
    g_rx_held = -1;
}

// Devuelve el siguiente bloque recibido (válido hasta la siguiente llamada).
static usb::Status rx_next(u8 **buf, u32 *got, u32 timeout_ms) {
    if (g_rx_held >= 0) {
        if (!usb::rx_repost(g_rx_held)) return usb::Status::Error;
        g_rx_held = -1;
    }
    if (!g_rx_running) {
        if (!usb::rx_start((void *const *)g_rx, RX_N, RX_SIZE)) return usb::Status::Error;
        g_rx_running = true;
    }
    int idx = -1;
    usb::Status st = usb::rx_wait(&idx, got, timeout_ms);
    if (st != usb::Status::Ok) { rx_reset(); return st; }
    g_rx_held = idx;
    *buf = g_rx[idx];
    return usb::Status::Ok;
}

static u64 now_ns() { return armTicksToNs(armGetSystemTick()); }

static void log_speed(const char *what, const std::string &path, u64 bytes, u64 t0, u64 sd_ns) {
    if (bytes < (8ULL << 20)) return; // solo archivos de 8 MB o más
    double secs = (now_ns() - t0) / 1e9, sd = sd_ns / 1e9;
    log_write("%s %.1f MB en %.2f s = %.1f MB/s | micro: %.2f s (%.1f MB/s) | %s", what, bytes / 1048576.0, secs,
              secs > 0 ? bytes / 1048576.0 / secs : 0.0, sd, sd > 0 ? bytes / 1048576.0 / sd : 0.0, path.c_str());
}

const char *link_name() {
    switch (usb::max_packet()) {
    case 0x400: return "USB 3 (5 Gb/s)";
    case 0x40: return "USB 1.1 (12 Mb/s)";
    default: return "USB 2.0 (480 Mb/s)";
    }
}

void status(char *out, size_t size, u64 *bytes) {
    mutexLock(&g_status_mtx);
    snprintf(out, size, "%s", g_status);
    mutexUnlock(&g_status_mtx);
    *bytes = g_bytes.load();
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    mutexLock(&g_status_mtx);
    va_list va;
    va_start(va, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, va);
    va_end(va);
    mutexUnlock(&g_status_mtx);
}

// ---------------- objetos ----------------
struct Obj {
    u32 storage;
    int save;        // índice de partida (solo STOR_SV), -1 en otro caso
    std::string rel; // ruta relativa a la raíz de la unidad ("" = raíz de la partida)
    u32 parent;      // 0 = raíz
    bool alive;
};
static std::vector<Obj> g_objs;
static std::unordered_map<std::string, u32> g_keys;

static std::string key_of(u32 st, int sv, const std::string &rel) {
    return std::to_string(st) + "|" + std::to_string(sv) + "|" + rel;
}

static u32 handle_for(u32 st, int sv, const std::string &rel, u32 parent) {
    std::string k = key_of(st, sv, rel);
    auto it = g_keys.find(k);
    if (it != g_keys.end()) {
        Obj &o = g_objs[it->second - 1];
        o.alive = true;
        o.parent = parent;
        return it->second;
    }
    g_objs.push_back({st, sv, rel, parent, true});
    u32 h = (u32)g_objs.size();
    g_keys[k] = h;
    return h;
}

static Obj *get_obj(u32 h) {
    if (h == 0 || h > g_objs.size()) return nullptr;
    Obj &o = g_objs[h - 1];
    return o.alive ? &o : nullptr;
}

static void reset_objects() {
    g_objs.clear();
    g_keys.clear();
}

static bool valid_storage(u32 st) { return st == STOR_SD || st == STOR_HB || st == STOR_SV; }

static bool base_path(u32 st, int sv, std::string &out) {
    if (st == STOR_SD) { out = "sdmc:/"; return true; }
    if (st == STOR_HB) { out = "sdmc:/switch/"; return true; }
    if (st == STOR_SV) {
        std::string dev;
        if (!saves::mount(sv, dev)) return false;
        out = dev + ":/";
        return true;
    }
    return false;
}

static bool full_path(const Obj &o, std::string &out) {
    std::string b;
    if (!base_path(o.storage, o.save, b)) return false;
    out = b + o.rel;
    return true;
}

static std::string join(const std::string &a, const std::string &b) { return a.empty() ? b : a + "/" + b; }

static std::string obj_name(const Obj &o) {
    if (o.rel.empty()) return o.storage == STOR_SV ? saves::name(o.save) : "";
    auto p = o.rel.rfind('/');
    return p == std::string::npos ? o.rel : o.rel.substr(p + 1);
}

static bool obj_stat(const Obj &o, bool &is_dir, u64 &size, time_t &mtime) {
    std::string p;
    if (!full_path(o, p)) return false;
    if (o.rel.empty()) { is_dir = true; size = 0; mtime = 0; return true; }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    is_dir = S_ISDIR(st.st_mode);
    size = is_dir ? 0 : (u64)st.st_size;
    mtime = st.st_mtime;
    return true;
}

static void touch_save(const Obj &o) {
    if (o.storage == STOR_SV) saves::commit(o.save);
}

// Borrado recursivo con las llamadas nativas del sistema de archivos. Registra cada
// entrada que no se pueda borrar (con su nombre en hexadecimal) para poder ver qué pasa.
static bool native_rm_dir(FsFileSystem *fs, const std::string &dir, int depth) {
    FsDir d;
    char path[FS_MAX_PATH] = {0};
    snprintf(path, sizeof(path), "%s", dir.c_str());
    Result rc = fsFsOpenDirectory(fs, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &d);
    if (R_FAILED(rc)) { log_write("mtp: borrar: OpenDirectory %s -> 0x%X", dir.c_str(), rc); return false; }
    std::vector<FsDirectoryEntry> ents;
    for (;;) {
        FsDirectoryEntry e[16];
        s64 n = 0;
        Result rr = fsDirRead(&d, &n, 16, e);
        if (R_FAILED(rr)) { log_write("mtp: borrar: leer %s fallo tras %u entradas -> 0x%X", dir.c_str(), (unsigned)ents.size(), rr); break; }
        if (n <= 0) break;
        ents.insert(ents.end(), e, e + n);
    }
    fsDirClose(&d);
    if (depth == 0) log_write("mtp: borrar: %s tiene %u entradas", dir.c_str(), (unsigned)ents.size());
    bool ok = true;
    for (auto &e : ents) {
        std::string child = dir + "/" + e.name;
        snprintf(path, sizeof(path), "%s", child.c_str());
        Result r;
        if (e.type == FsDirEntryType_Dir) r = native_rm_dir(fs, child, depth + 1) ? 0 : 1;
        else r = fsFsDeleteFile(fs, path);
        if (R_FAILED(r)) {
            ok = false;
            char hex[3 * 48 + 1] = {0};
            for (int k = 0; k < 48 && e.name[k]; k++) snprintf(hex + 3 * k, 4, "%02X ", (u8)e.name[k]);
            log_write("mtp: borrar: no se pudo borrar '%s' (0x%X) nombre: %s", e.name, r, hex);
        }
    }
    snprintf(path, sizeof(path), "%s", dir.c_str());
    rc = fsFsDeleteDirectory(fs, path);
    if (R_FAILED(rc)) { log_write("mtp: borrar: DeleteDirectory %s -> 0x%X", dir.c_str(), rc); return false; }
    return ok;
}

// Borra un archivo o una carpeta con todo su contenido. Primero se leen todos los
// nombres y después se borra: borrar mientras se lee la carpeta salta entradas en la
// micro y la carpeta quedaba sin vaciar ("No tiene permiso para eliminar").
static bool rm_rf(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { log_write("mtp: borrar: no existe %s (errno %d)", p.c_str(), errno); return false; }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(p.c_str()) == 0) return true;
        log_write("mtp: borrar: unlink %s -> errno %d", p.c_str(), errno);
        return false;
    }
    // Carpeta: lo hace el propio sistema de archivos de la consola, de una vez.
    size_t colon = p.find(":/");
    if (colon != std::string::npos) {
        std::string dev = p.substr(0, colon), rest = p.substr(colon + 1);
        if (FsFileSystem *fs = fsdevGetDeviceFileSystem(dev.c_str())) {
            char fp[FS_MAX_PATH] = {0};
            snprintf(fp, sizeof(fp), "%s", rest.c_str());
            Result rc = fsFsDeleteDirectoryRecursively(fs, fp);
            if (R_SUCCEEDED(rc)) return true;
            log_write("mtp: borrar: DeleteDirectoryRecursively %s -> 0x%X", p.c_str(), rc);
            if (native_rm_dir(fs, rest, 0)) return true;
        } else {
            log_write("mtp: borrar: sin sistema de archivos para '%s'", dev.c_str());
        }
    }
    // Plan B: a mano (primero leer todos los nombres, después borrar).
    for (int pass = 0; pass < 3; pass++) {
        std::vector<std::string> names;
        if (DIR *d = opendir(p.c_str())) {
            while (auto *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                names.emplace_back(e->d_name);
            }
            closedir(d);
        }
        if (names.empty()) break;
        for (auto &n : names) rm_rf(p + "/" + n);
    }
    if (rmdir(p.c_str()) == 0) return true;
    log_write("mtp: borrar: rmdir %s -> errno %d", p.c_str(), errno);
    return false;
}

// Tras renombrar/mover un objeto, actualiza las rutas de sus hijos ya conocidos.
static void reparent_paths(const Obj &o, const std::string &old_rel, const std::string &new_rel) {
    std::string pre = old_rel + "/";
    for (u32 i = 0; i < g_objs.size(); i++) {
        Obj &c = g_objs[i];
        if (c.storage != o.storage || c.save != o.save) continue;
        if (c.rel == old_rel || c.rel.compare(0, pre.size(), pre) == 0) {
            g_keys.erase(key_of(c.storage, c.save, c.rel));
            c.rel = new_rel + c.rel.substr(old_rel.size());
            g_keys[key_of(c.storage, c.save, c.rel)] = i + 1;
        }
    }
}

// ---------------- codificación ----------------
static std::u16string utf8_to_16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        u32 c = (u8)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n == 2) c &= 0x1F; else if (n == 3) c &= 0x0F; else if (n == 4) c &= 0x07;
        for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | ((u8)s[i + k] & 0x3F);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back((char16_t)(0xD800 + (c >> 10)));
            out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else out.push_back((char16_t)c);
    }
    return out;
}

static std::string utf16_to_8(const std::u16string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

struct W {
    std::vector<u8> v;
    void u8_(u8 x) { v.push_back(x); }
    void u16_(u16 x) { u8_(x & 0xFF); u8_(x >> 8); }
    void u32_(u32 x) { u16_(x & 0xFFFF); u16_(x >> 16); }
    void u64_(u64 x) { u32_((u32)x); u32_((u32)(x >> 32)); }
    void str(const std::string &s) {
        std::u16string u = utf8_to_16(s);
        if (u.empty()) { u8_(0); return; }
        if (u.size() > 254) u.resize(254);
        u8_((u8)(u.size() + 1));
        for (char16_t c : u) u16_(c);
        u16_(0);
    }
    void a16(std::initializer_list<u16> l) { u32_((u32)l.size()); for (u16 x : l) u16_(x); }
    void a32(const std::vector<u32> &l) { u32_((u32)l.size()); for (u32 x : l) u32_(x); }
};

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static std::string rd_str(const u8 *p, size_t avail, size_t *used) {
    if (avail < 1) { if (used) *used = 0; return ""; }
    u8 n = p[0];
    size_t need = 1 + (size_t)n * 2;
    if (used) *used = need;
    if (need > avail) return "";
    std::u16string u;
    for (u8 i = 0; i < n; i++) {
        char16_t c = rd16(p + 1 + i * 2);
        if (!c) break;
        u.push_back(c);
    }
    return utf16_to_8(u);
}

// Nombre seguro para la micro de la Switch 2: un archivo cuyo nombre lleva U+FFFD
// (el "carácter desconocido" que deja Windows cuando un nombre se estropeó) o
// caracteres de control rompe la lectura de TODA su carpeta: deja de listarse y no
// se puede borrar. Se cambian por '_' (también los prohibidos en FAT).
static std::string safe_name(const std::string &in) {
    std::u16string u = utf8_to_16(in), o;
    for (char16_t c : u) {
        bool bad = c < 0x20 || c == 0xFFFD || c == 0xFFFE || c == 0xFFFF ||
                   c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        o.push_back(bad ? u'_' : c);
    }
    std::string r = utf16_to_8(o);
    while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
    return r.empty() ? std::string("_") : r;
}

static std::string mtp_date(time_t t) {
    if (t == 0) return "";
    char b[32];
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(b, sizeof(b), "%Y%m%dT%H%M%S", &tmv);
    return b;
}

// ---------------- transporte ----------------
struct Cmd {
    u16 code;
    u32 tid;
    u32 p[5];
    int np;
};

static bool looks_like_command(const u8 *b, u32 got) {
    return got >= 12 && got <= 32 && rd32(b) == got && rd16(b + 4) == CT_COMMAND;
}

// Escribe y traduce el estado: false solo si el USB murió; si el PC no lee
// (canceló), marca g_aborted y devuelve true.
static bool usb_write(const void *b, u32 n) {
    if (g_aborted) return true;
    usb::Status st = usb::write(b, n, IO_TIMEOUT_MS);
    if (st == usb::Status::Timeout) { g_aborted = true; log_write("mtp: cancelado: el PC dejo de leer %u s", IO_TIMEOUT_MS / 1000); return true; }
    return st == usb::Status::Ok;
}

static u16 g_cur_op = 0;

static bool send_response(u32 tid, u16 code, std::initializer_list<u32> params = {}) {
    if (g_aborted) return true; // el PC abandonó esta operación: no se responde
    if (code != RC_OK) log_write("mtp: op 0x%04X -> respuesta 0x%04X", g_cur_op, code);
    u32 len = 12 + 4 * (u32)params.size();
    u8 *b = g_cmd;
    memset(b, 0, len);
    b[0] = len & 0xFF; b[1] = (len >> 8) & 0xFF;
    b[4] = CT_RESPONSE; b[6] = code & 0xFF; b[7] = code >> 8;
    memcpy(b + 8, &tid, 4);
    int i = 0;
    for (u32 x : params) memcpy(b + 12 + 4 * i++, &x, 4);
    return usb_write(b, len);
}

static void put_header(u8 *b, u64 total, u16 code, u32 tid) {
    u32 len = total > 0xFFFFFFFFULL ? 0xFFFFFFFF : (u32)total;
    memcpy(b, &len, 4);
    b[4] = CT_DATA; b[5] = 0;
    b[6] = code & 0xFF; b[7] = code >> 8;
    memcpy(b + 8, &tid, 4);
}

static bool send_zlp_if_needed(u64 total) {
    if (total % usb::max_packet() == 0) return usb_write(g_buf, 0);
    return true;
}

static bool send_data(const Cmd &c, const std::vector<u8> &payload) {
    u64 total = 12 + payload.size();
    put_header(g_buf, total, c.code, c.tid);
    size_t off = 0;
    u32 fill = 12;
    for (;;) {
        u32 n = (u32)std::min<size_t>(BUF_SIZE - fill, payload.size() - off);
        if (n) memcpy(g_buf + fill, payload.data() + off, n);
        off += n;
        fill += n;
        if (!usb_write(g_buf, fill)) return false;
        if (g_aborted) return true;
        if (off >= payload.size()) break;
        fill = 0;
    }
    return send_zlp_if_needed(total);
}

// Recibe una fase de datos del PC. sink() recibe los trozos de carga útil
// mientras ya se está recibiendo el siguiente trozo por USB (doble buffer).
// false = USB muerto. Si el PC cancela, g_aborted = true.
static bool recv_data(const std::function<bool(const u8 *, size_t)> &sink) {
    u32 got = 0;
    u8 *cur = nullptr;
    usb::Status st;
    do {
        st = rx_next(&cur, &got, 0);
    } while (st == usb::Status::Ok && got == 0); // ZLP sobrante de la transferencia anterior
    if (st == usb::Status::Timeout) { g_aborted = true; log_write("mtp: cancelado por el PC antes de los datos"); return true; }
    if (st != usb::Status::Ok) return false;
    if (rd16(cur + 4) != CT_DATA && looks_like_command(cur, got)) {
        memcpy(g_pending_cmd, cur, got);
        g_pending_len = got;
        g_aborted = true;
        log_write("mtp: cancelado: llego el comando 0x%04X en vez de los datos", rd16(cur + 6));
        return true;
    }
    if (got < 12) {
        g_aborted = true;
        log_write("mtp: cancelado: primer bloque de datos de solo %u bytes (tipo %u)", got, got >= 6 ? rd16(cur + 4) : 0);
        return true;
    }
    u32 len = rd32(cur);
    bool unknown = len == 0xFFFFFFFF;
    u64 recvd = got;
    log_debug("mtp: datos: contenedor tipo %u codigo 0x%04X largo %u, primer bloque %u", rd16(cur + 4), rd16(cur + 6), len, got);
    u32 cur_n = got, cur_off = 12;
    bool more = unknown ? got == RX_SIZE : recvd < len;
    bool ok = true;
    for (;;) {
        // la siguiente lectura ya está publicada (rx_next): se escribe en la micro mientras llega
        if (ok) ok = sink(cur + cur_off, cur_n - cur_off);
        g_bytes += cur_n - cur_off;
        if (!more) break;
        u32 n = 0;
        u8 *nxt = nullptr;
        st = rx_next(&nxt, &n, IO_TIMEOUT_MS);
        if (st == usb::Status::Error) return false;
        if (st == usb::Status::Timeout) { g_aborted = true; log_write("mtp: cancelado: el PC dejo de enviar datos (llevaba %llu de %u bytes)", (unsigned long long)recvd, len); return true; }
        if (n == 0) break;
        if (looks_like_command(nxt, n) && (unknown || len - recvd != n)) {
            memcpy(g_pending_cmd, nxt, n);
            g_pending_len = n;
            g_aborted = true;
            log_write("mtp: cancelado: llego el comando 0x%04X a mitad de la copia", rd16(nxt + 6));
            return true;
        }
        recvd += n;
        cur = nxt;
        cur_n = n;
        cur_off = 0;
        more = unknown ? n == RX_SIZE : recvd < len;
    }
    return true;
}

// ---------------- operaciones ----------------
static bool op_device_info(const Cmd &c) {
    W w;
    w.u16_(100);
    w.u32_(6);
    w.u16_(100);
    w.str("microsoft.com: 1.0;");
    w.u16_(0);
    // Mismo conjunto que anuncia libhaze (Sphaira), que funciona con el Explorador de
    // Windows. Windows cambia su forma de copiar según lo que se anuncia aquí.
    w.a16({OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession, OP_GetStorageIDs, OP_GetStorageInfo,
           OP_GetObjectHandles, OP_GetObjectInfo, OP_GetObject, OP_SendObjectInfo, OP_SendObject,
           OP_DeleteObject, OP_GetObjectPropsSupported, OP_GetObjectPropDesc, OP_GetObjectPropValue,
           OP_SetObjectPropValue, OP_GetObjectPropList, OP_SendObjectPropList});
    w.a16({});
    w.a16({});
    w.a16({});
    w.a16({FMT_Undefined, FMT_Association});
    w.str("Nintendo");
    w.str("Switch 2 (USBS2)");
    w.str("1.0");
    w.str("USBS2-0001");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_storage_ids(const Cmd &c) {
    W w;
    w.a32({STOR_SD, STOR_HB, STOR_SV});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_storage_info(const Cmd &c) {
    u32 st = c.p[0];
    if (!valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
    u64 total = 0, free_ = 0;
    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        total = (u64)vfs.f_blocks * vfs.f_frsize;
        free_ = (u64)vfs.f_bavail * vfs.f_frsize;
    }
    if (st == STOR_SV) { total = 1ULL << 30; free_ = 1ULL << 30; } // valor nominal: el límite real lo pone cada partida
    W w;
    w.u16_(st == STOR_SV ? 0x0003 : 0x0004);
    w.u16_(0x0002);
    w.u16_(0x0000);
    w.u64_(total);
    w.u64_(free_);
    w.u32_(0xFFFFFFFF);
    w.str(st == STOR_SD ? "Tarjeta SD" : st == STOR_HB ? "Homebrew (sdmc-switch)" : "Partidas guardadas");
    w.str(st == STOR_SD ? "sdmc" : st == STOR_HB ? "switch" : "saves");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static void list_children(u32 st, u32 parent_h, std::vector<u32> &out) {
    if (parent_h == ROOT || parent_h == 0) {
        if (st == STOR_SV) {
            for (int i = 0; i < saves::count(); i++) out.push_back(handle_for(STOR_SV, i, "", 0));
            return;
        }
        std::string base;
        if (!base_path(st, -1, base)) return;
        if (DIR *d = opendir(base.c_str())) {
            while (auto *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                out.push_back(handle_for(st, -1, e->d_name, 0));
            }
            closedir(d);
        }
        return;
    }
    Obj *o = get_obj(parent_h);
    if (!o) return;
    Obj parent = *o; // copia: handle_for puede reubicar el vector
    std::string p;
    if (!full_path(parent, p)) return;
    if (DIR *d = opendir(p.c_str())) {
        while (auto *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            out.push_back(handle_for(parent.storage, parent.save, join(parent.rel, e->d_name), parent_h));
        }
        closedir(d);
    }
}

static bool op_object_handles(const Cmd &c) {
    u32 st = c.p[0], parent = c.np > 2 ? c.p[2] : 0;
    if (st != 0xFFFFFFFF && !valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
    if (parent != 0 && parent != ROOT && !get_obj(parent)) return send_response(c.tid, RC_InvalidParentObject);
    std::vector<u32> hs;
    if (st == 0xFFFFFFFF && (parent == 0 || parent == ROOT)) {
        for (u32 s : {STOR_SD, STOR_HB, STOR_SV}) list_children(s, ROOT, hs);
    } else {
        if (st == 0xFFFFFFFF) st = get_obj(parent)->storage;
        list_children(st, parent, hs);
    }
    W w;
    w.a32(hs);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_object_info(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    bool dir; u64 size; time_t mt;
    if (!obj_stat(o, dir, size, mt)) return send_response(c.tid, RC_InvalidObjectHandle);
    W w;
    w.u32_(o.storage);
    w.u16_(dir ? FMT_Association : FMT_Undefined);
    w.u16_(0);
    w.u32_(size > 0xFFFFFFFFULL ? 0xFFFFFFFF : (u32)size);
    w.u16_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0);
    w.u32_(o.parent);
    w.u16_(dir ? 0x0001 : 0x0000);
    w.u32_(0); w.u32_(0);
    w.str(obj_name(o));
    w.str(mtp_date(mt));
    w.str(mtp_date(mt));
    w.str("");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool stream_file(const Cmd &c, const std::string &path, u64 offset, u64 len) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return send_response(c.tid, RC_AccessDenied);
    if (offset) lseek(fd, (off_t)offset, SEEK_SET);
    u64 total = 12 + len;
    u64 left = len;
    u64 t0 = now_ns(), sd_ns = 0;
    auto fill = [&](u8 *b, u32 start) -> u32 {
        u32 want = (u32)std::min<u64>(BUF_SIZE - start, left);
        u64 t = now_ns();
        u32 got = 0;
        while (got < want) {
            ssize_t r = read(fd, b + start + got, want - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        sd_ns += now_ns() - t;
        if (got < want) memset(b + start + got, 0, want - got); // archivo truncado: rellena para no romper el protocolo
        left -= want;
        return start + want;
    };
    set_status("Enviando %s", path.c_str());
    put_header(g_buf, total, c.code, c.tid);
    u8 *cur = g_buf;
    u32 n = fill(cur, 12);
    bool dead = false;
    for (;;) {
        if (!usb::begin_write(cur, n)) { dead = true; break; }
        u8 *nxt = cur == g_buf ? g_buf2 : g_buf;
        u32 nn = left ? fill(nxt, 0) : 0; // lee de la micro mientras el USB envía
        usb::Status st = usb::end_write(IO_TIMEOUT_MS);
        if (st == usb::Status::Error) { dead = true; break; }
        if (st == usb::Status::Timeout) { g_aborted = true; log_write("mtp: cancelado: el PC dejo de leer %u s", IO_TIMEOUT_MS / 1000); break; }
        g_bytes += n;
        if (nn == 0) break;
        cur = nxt;
        n = nn;
    }
    close(fd);
    set_status("Conectado al PC");
    if (dead) return false;
    if (g_aborted) return true;
    log_speed("PC <- consola", path, len, t0, sd_ns);
    if (!send_zlp_if_needed(total)) return false;
    if (c.code == OP_GetPartialObject) return send_response(c.tid, RC_OK, {(u32)len});
    return send_response(c.tid, RC_OK);
}

static bool op_get_object(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    bool dir; u64 size; time_t mt;
    std::string p;
    if (!obj_stat(o, dir, size, mt) || dir || !full_path(o, p)) return send_response(c.tid, RC_InvalidObjectHandle);
    if (c.code == OP_GetPartialObject) {
        u64 off = c.p[1], max = c.p[2];
        if (off > size) off = size;
        return stream_file(c, p, off, std::min<u64>(max, size - off));
    }
    return stream_file(c, p, 0, size);
}

static bool op_delete(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied); // raíz de una partida
    std::string p;
    if (!full_path(*op, p)) return send_response(c.tid, RC_GeneralError);
    bool ok = rm_rf(p);
    if (!ok) log_write("mtp: no se pudo borrar %s", p.c_str());
    touch_save(*op);
    if (ok) op->alive = false;
    return send_response(c.tid, ok ? RC_OK : RC_AccessDenied);
}

// estado entre SendObjectInfo y SendObject
static struct { bool pending = false; u32 handle = 0; std::string path; u64 size = 0; } g_send;

// Crea el archivo/carpeta pedido por SendObjectInfo o SendObjectPropList y responde.
static bool create_object(const Cmd &c, u32 st, u32 parent_h, const std::string &name_in, u16 fmt, u64 size) {
    const std::string name = name_in.empty() ? name_in : safe_name(name_in);
    g_send.pending = false;
    if (name.empty() || name.find('/') != std::string::npos) return send_response(c.tid, RC_NoValidObjectInfo);

    Obj parent{st, -1, "", 0, true};
    if (parent_h != ROOT && parent_h != 0) {
        Obj *po = get_obj(parent_h);
        if (!po) return send_response(c.tid, RC_InvalidParentObject);
        parent = *po;
        st = parent.storage;
    } else {
        parent_h = 0;
        if (!valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
        if (st == STOR_SV) return send_response(c.tid, RC_AccessDenied); // no se crean partidas nuevas
    }

    std::string pp;
    if (!full_path(parent, pp)) return send_response(c.tid, RC_GeneralError);
    std::string rel = join(parent.rel, name);
    std::string path = (pp.empty() || pp.back() == '/') ? pp + name : pp + "/" + name;
    if (name != name_in) {
        // Limpia la copia anterior con el nombre roto, si la hay (la que bloqueaba la carpeta).
        std::string bad = (pp.empty() || pp.back() == '/') ? pp + name_in : pp + "/" + name_in;
        int r = fmt == FMT_Association ? rmdir(bad.c_str()) : unlink(bad.c_str());
        log_write("mtp: nombre corregido '%s' (antes con caracter invalido; borrar el viejo: %s)", name.c_str(), r == 0 ? "borrado" : "no estaba");
    }

    if (fmt == FMT_Association) {
        if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) return send_response(c.tid, RC_AccessDenied);
        touch_save(parent);
    }
    u32 h = handle_for(st, parent.save, rel, parent_h);
    if (fmt != FMT_Association) {
        g_send.pending = true;
        g_send.handle = h;
        g_send.path = path;
        g_send.size = size;
    }
    // Como libhaze: carpeta padre 0 cuando el objeto queda en la raíz de la unidad.
    return send_response(c.tid, RC_OK, {st, parent_h, h});
}

static bool op_send_object_info(const Cmd &c) {
    u32 st = c.p[0], parent_h = c.np > 1 ? c.p[1] : ROOT;
    std::vector<u8> data;
    if (!recv_data([&](const u8 *p, size_t n) { data.insert(data.end(), p, p + n); return data.size() < 0x10000; }))
        return false;
    if (g_aborted) return true;
    g_send.pending = false;
    if (data.size() < 53) return send_response(c.tid, RC_NoValidObjectInfo);
    u16 fmt = rd16(&data[4]);
    u32 sz = rd32(&data[8]);
    std::string name = rd_str(&data[52], data.size() - 52, nullptr);
    log_debug("mtp: SendObjectInfo '%s' formato 0x%04X tamano %u (almacen 0x%X, padre 0x%X)", name.c_str(), fmt, sz, st, parent_h);
    return create_object(c, st, parent_h, name, fmt, sz == 0xFFFFFFFF ? 0 : sz);
}

// Tamaño en bytes de un valor de tipo MTP (cadenas aparte).
static size_t dt_size(u16 t) {
    switch (t) {
    case 0x0001: case 0x0002: return 1;
    case 0x0003: case 0x0004: return 2;
    case 0x0005: case 0x0006: return 4;
    case 0x0007: case 0x0008: return 8;
    case 0x0009: case 0x000A: return 16;
    }
    return 0;
}

// SendObjectPropList: como SendObjectInfo, pero el tamaño (64 bits) va en los
// parámetros y el nombre en una lista de propiedades.
static bool op_send_object_prop_list(const Cmd &c) {
    u32 st = c.p[0], parent_h = c.np > 1 ? c.p[1] : ROOT;
    u16 fmt = (u16)(c.np > 2 ? c.p[2] : FMT_Undefined);
    u64 size = ((u64)(c.np > 3 ? c.p[3] : 0) << 32) | (c.np > 4 ? c.p[4] : 0);
    std::vector<u8> data;
    if (!recv_data([&](const u8 *p, size_t n) { data.insert(data.end(), p, p + n); return data.size() < 0x10000; }))
        return false;
    if (g_aborted) return true;
    std::string name;
    if (data.size() >= 4) {
        u32 count = rd32(&data[0]);
        size_t off = 4;
        for (u32 i = 0; i < count && off + 8 <= data.size(); i++) {
            u16 prop = rd16(&data[off + 4]);
            u16 type = rd16(&data[off + 6]);
            off += 8;
            if (type == DT_STR) {
                size_t used = 0;
                std::string v = rd_str(&data[off], data.size() - off, &used);
                if (prop == PROP_ObjectFileName) name = v;
                off += used ? used : 1;
            } else {
                size_t n = dt_size(type);
                if (!n) break;
                off += n;
            }
        }
    }
    log_debug("mtp: SendObjectPropList '%s' formato 0x%04X tamano %llu (almacen 0x%X, padre 0x%X)", name.c_str(), fmt,
              (unsigned long long)size, st, parent_h);
    return create_object(c, st, parent_h, name, fmt, size);
}

static bool op_send_object(const Cmd &c) {
    if (!g_send.pending) {
        if (!recv_data([](const u8 *, size_t) { return false; })) return false;
        return send_response(c.tid, RC_NoValidObjectInfo);
    }
    g_send.pending = false;
    int fd = open(g_send.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool werr = fd < 0;
    if (fd < 0) log_write("mtp: SendObject: no se pudo crear %s (errno %d)", g_send.path.c_str(), errno);
    else log_debug("mtp: SendObject -> %s", g_send.path.c_str());
    // Reservar el tamaño final de una vez: la micro no tiene que ir agrandando el archivo.
    bool presized = !werr && g_send.size && ftruncate(fd, (off_t)g_send.size) == 0;
    u64 written = 0, t0 = now_ns(), sd_ns = 0;
    set_status("Recibiendo %s", g_send.path.c_str());
    bool ok = recv_data([&](const u8 *p, size_t n) {
        if (werr) return false;
        u64 t = now_ns();
        size_t done = 0;
        while (done < n) {
            ssize_t r = write(fd, p + done, n - done);
            if (r <= 0) { werr = true; break; }
            done += (size_t)r;
        }
        sd_ns += now_ns() - t;
        written += done;
        return !werr;
    });
    if (fd >= 0) {
        if (presized && !g_aborted && written != g_send.size) ftruncate(fd, (off_t)written);
        close(fd);
    }
    bool problem = werr || g_aborted || !ok || (g_send.size && written != g_send.size);
    if (problem || g_log_verbose) log_write("mtp: SendObject fin: %llu de %llu bytes, error escritura: %s, cancelado: %s, usb ok: %s",
              (unsigned long long)written, (unsigned long long)g_send.size, werr ? "si" : "no", g_aborted ? "si" : "no", ok ? "si" : "no");
    if (!g_aborted && !werr) log_speed("PC -> consola", g_send.path, written, t0, sd_ns);
    Obj *o = get_obj(g_send.handle);
    if (g_aborted) {
        unlink(g_send.path.c_str()); // copia cancelada: no dejar el archivo a medias
        if (o) o->alive = false;
        log_write("mtp: copia cancelada, borrado %s", g_send.path.c_str());
    }
    if (o) touch_save(*o);
    set_status("Conectado al PC");
    if (!ok) return false;
    return send_response(c.tid, werr ? RC_StoreFull : RC_OK);
}

static bool op_move(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied);
    u32 dst_st = c.p[1], dst_parent = c.p[2];
    Obj dst{dst_st, -1, "", 0, true};
    if (dst_parent && dst_parent != ROOT) {
        Obj *d = get_obj(dst_parent);
        if (!d) return send_response(c.tid, RC_InvalidParentObject);
        dst = *d;
    } else {
        dst_parent = 0;
        if (dst_st == STOR_SV) return send_response(c.tid, RC_AccessDenied);
    }
    // solo dentro de la misma unidad (y de la misma partida)
    if (dst.storage != op->storage || dst.save != op->save) return send_response(c.tid, RC_OperationNotSupported);
    std::string from, base;
    if (!full_path(*op, from) || !base_path(op->storage, op->save, base)) return send_response(c.tid, RC_GeneralError);
    std::string new_rel = join(dst.rel, obj_name(*op));
    std::string old_rel = op->rel;
    if (rename(from.c_str(), (base + new_rel).c_str()) != 0) return send_response(c.tid, RC_AccessDenied);
    Obj copy = *op;
    reparent_paths(copy, old_rel, new_rel);
    if (Obj *o2 = get_obj(c.p[0])) o2->parent = dst_parent;
    touch_save(copy);
    return send_response(c.tid, RC_OK);
}

static void prop_desc(W &w, u16 prop) {
    w.u16_(prop);
    switch (prop) {
    case PROP_StorageID: case PROP_ParentObject: w.u16_(DT_UINT32); w.u8_(0); w.u32_(0); break;
    case PROP_ObjectFormat: case PROP_ProtectionStatus: w.u16_(DT_UINT16); w.u8_(0); w.u16_(0); break;
    case PROP_ObjectSize: w.u16_(DT_UINT64); w.u8_(0); w.u64_(0); break;
    case PROP_PersistentUID: w.u16_(DT_UINT128); w.u8_(0); w.u64_(0); w.u64_(0); break;
    case PROP_ObjectFileName: w.u16_(DT_STR); w.u8_(1); w.str(""); break;
    default: w.u16_(DT_STR); w.u8_(0); w.str(""); break;
    }
    w.u32_(0);
    w.u8_(0);
}

static bool known_prop(u16 p) {
    switch (p) {
    case PROP_StorageID: case PROP_ObjectFormat: case PROP_ObjectSize:
    case PROP_ObjectFileName: case PROP_ParentObject: case PROP_PersistentUID: return true;
    }
    return false;
}

static bool op_props_supported(const Cmd &c) {
    W w;
    w.a16({PROP_StorageID, PROP_ObjectFormat, PROP_ObjectSize, PROP_ObjectFileName, PROP_ParentObject,
           PROP_PersistentUID});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_prop_desc(const Cmd &c) {
    if (!known_prop((u16)c.p[0])) return send_response(c.tid, RC_InvalidObjectPropCode);
    W w;
    prop_desc(w, (u16)c.p[0]);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static u16 prop_type(u16 prop) {
    switch (prop) {
    case PROP_StorageID: case PROP_ParentObject: return DT_UINT32;
    case PROP_ObjectFormat: case PROP_ProtectionStatus: return DT_UINT16;
    case PROP_ObjectSize: return DT_UINT64;
    case PROP_PersistentUID: return DT_UINT128;
    default: return DT_STR;
    }
}

static void put_prop_value(W &w, u32 h, const Obj &o, u16 prop, bool dir, u64 size, time_t mt) {
    switch (prop) {
    case PROP_StorageID: w.u32_(o.storage); break;
    case PROP_ObjectFormat: w.u16_(dir ? FMT_Association : FMT_Undefined); break;
    case PROP_ProtectionStatus: w.u16_(0); break;
    case PROP_ObjectSize: w.u64_(size); break;
    case PROP_ObjectFileName: case PROP_Name: w.str(obj_name(o)); break;
    case PROP_DateModified: w.str(mtp_date(mt)); break;
    case PROP_ParentObject: w.u32_(o.parent); break;
    case PROP_PersistentUID: w.u64_(h); w.u64_(o.storage); break;
    }
}

static const u16 ALL_PROPS[] = {PROP_StorageID, PROP_ObjectFormat, PROP_ObjectSize, PROP_ObjectFileName,
                                PROP_ParentObject, PROP_PersistentUID};

static bool op_prop_value(const Cmd &c) {
    u32 h = c.p[0];
    Obj *op = get_obj(h);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    u16 prop = (u16)c.p[1];
    if (!known_prop(prop)) return send_response(c.tid, RC_InvalidObjectPropCode);
    bool dir; u64 size; time_t mt;
    if (!obj_stat(o, dir, size, mt)) return send_response(c.tid, RC_InvalidObjectHandle);
    W w;
    put_prop_value(w, h, o, prop, dir, size, mt);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

// GetObjectPropList: todas las propiedades de un objeto (profundidad 0) o de
// todos los hijos de una carpeta (profundidad 1) en una sola respuesta. Windows
// lo usa si está disponible y así no pregunta propiedad por propiedad.
static bool op_prop_list(const Cmd &c) {
    u32 h = c.p[0], prop = c.np > 2 ? c.p[2] : 0xFFFFFFFF, group = c.np > 3 ? c.p[3] : 0, depth = c.np > 4 ? c.p[4] : 0;
    if (prop == 0 && group != 0) return send_response(c.tid, RC_SpecificationByGroupUnsupported);
    if (prop != 0 && prop != 0xFFFFFFFF && !known_prop((u16)prop)) return send_response(c.tid, RC_InvalidObjectPropCode);

    std::vector<u32> hs;
    if (depth == 0) {
        if (h == 0 || h == ROOT) return send_response(c.tid, RC_SpecificationByDepthUnsupported);
        if (!get_obj(h)) return send_response(c.tid, RC_InvalidObjectHandle);
        hs.push_back(h);
    } else if (depth == 1) {
        if (h == 0 || h == ROOT) {
            for (u32 st : {STOR_SD, STOR_HB, STOR_SV}) list_children(st, ROOT, hs);
        } else {
            Obj *po = get_obj(h);
            if (!po) return send_response(c.tid, RC_InvalidObjectHandle);
            list_children(po->storage, h, hs);
        }
    } else {
        return send_response(c.tid, RC_SpecificationByDepthUnsupported);
    }

    W w;
    w.u32_(0); // número de elementos, se rellena al final
    u32 count = 0;
    for (u32 oh : hs) {
        Obj *op = get_obj(oh);
        if (!op) continue;
        Obj o = *op;
        bool dir; u64 size; time_t mt;
        if (!obj_stat(o, dir, size, mt)) continue;
        for (u16 pc : ALL_PROPS) {
            if (prop != 0 && prop != 0xFFFFFFFF && pc != prop) continue;
            w.u32_(oh);
            w.u16_(pc);
            w.u16_(prop_type(pc));
            put_prop_value(w, oh, o, pc, dir, size, mt);
            count++;
        }
    }
    memcpy(w.v.data(), &count, 4);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_set_prop_value(const Cmd &c) {
    std::vector<u8> data;
    if (!recv_data([&](const u8 *p, size_t n) { data.insert(data.end(), p, p + n); return data.size() < 0x1000; }))
        return false;
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if ((u16)c.p[1] != PROP_ObjectFileName) return send_response(c.tid, RC_AccessDenied);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied);
    std::string name = data.empty() ? "" : rd_str(data.data(), data.size(), nullptr);
    if (name.empty() || name.find('/') != std::string::npos) return send_response(c.tid, RC_GeneralError);
    name = safe_name(name);
    std::string from, base;
    if (!full_path(*op, from) || !base_path(op->storage, op->save, base)) return send_response(c.tid, RC_GeneralError);
    std::string old_rel = op->rel;
    auto slash = old_rel.rfind('/');
    std::string new_rel = slash == std::string::npos ? name : old_rel.substr(0, slash + 1) + name;
    if (rename(from.c_str(), (base + new_rel).c_str()) != 0) return send_response(c.tid, RC_AccessDenied);
    Obj copy = *op;
    reparent_paths(copy, old_rel, new_rel);
    touch_save(copy);
    return send_response(c.tid, RC_OK);
}

static bool op_dev_prop(const Cmd &c) {
    if ((u16)c.p[0] != DPROP_FriendlyName) return send_response(c.tid, RC_DevicePropNotSupported);
    W w;
    if (c.code == OP_GetDevicePropDesc) {
        w.u16_(DPROP_FriendlyName);
        w.u16_(DT_STR);
        w.u8_(0);
        w.str("Switch 2");
        w.str("Switch 2");
        w.u8_(0);
    } else {
        w.str("Switch 2");
    }
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_refs(const Cmd &c) {
    W w;
    w.a32({});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool dispatch(const Cmd &c) {
    g_aborted = false;
    g_cur_op = c.code;
    if (c.code != OP_GetObjectPropValue && c.code != OP_GetObjectInfo)
        log_debug("mtp: op 0x%04X tid %u p0 0x%X", c.code, c.tid, c.np ? c.p[0] : 0);
    if (c.code == OP_GetDeviceInfo) return op_device_info(c);
    if (c.code == OP_OpenSession) {
        // Si el PC reabre la sesión (p. ej. tras cancelar), se reinicia sin error.
        g_session = true;
        g_send.pending = false;
        reset_objects();
        saves::scan();
        set_status("Conectado al PC (%d partidas)", saves::count());
        return send_response(c.tid, RC_OK);
    }
    if (!g_session) return send_response(c.tid, RC_SessionNotOpen);
    switch (c.code) {
    case OP_CloseSession:
        g_session = false;
        saves::unmount_all();
        set_status("Sesión cerrada");
        return send_response(c.tid, RC_OK);
    case OP_GetStorageIDs: return op_storage_ids(c);
    case OP_GetStorageInfo: return op_storage_info(c);
    case OP_GetObjectHandles: return op_object_handles(c);
    case OP_GetObjectInfo: return op_object_info(c);
    case OP_GetObject: case OP_GetPartialObject: return op_get_object(c);
    case OP_DeleteObject: return op_delete(c);
    case OP_SendObjectInfo: return op_send_object_info(c);
    case OP_SendObject: return op_send_object(c);
    case OP_MoveObject: return op_move(c);
    case OP_GetObjectPropsSupported: return op_props_supported(c);
    case OP_GetObjectPropDesc: return op_prop_desc(c);
    case OP_GetObjectPropValue: return op_prop_value(c);
    case OP_SetObjectPropValue: return op_set_prop_value(c);
    case OP_GetDevicePropDesc: case OP_GetDevicePropValue: return op_dev_prop(c);
    case OP_GetObjectReferences: return op_refs(c);
    case OP_GetObjectPropList: return op_prop_list(c);
    case OP_SendObjectPropList: return op_send_object_prop_list(c);
    default: return send_response(c.tid, RC_OperationNotSupported);
    }
}

static void session_loop() {
    while (!g_quit->load()) {
        u32 got = 0;
        if (g_pending_len) {
            got = g_pending_len;
            memcpy(g_cmd, g_pending_cmd, got);
            g_pending_len = 0;
        } else {
            usb::clear_cancel();
            u8 *b = nullptr;
            usb::Status st = rx_next(&b, &got, 0);
            if (st == usb::Status::Timeout) { usb::clear_cancel(); continue; } // cancelación sin operación en curso
            if (st != usb::Status::Ok) return;
            if (got > 0x1000) got = 0x1000;
            memcpy(g_cmd, b, got);
        }
        if (got == 0) continue;
        if (got < 12 || rd16(g_cmd + 4) != CT_COMMAND) continue;
        Cmd c{};
        c.code = rd16(g_cmd + 6);
        c.tid = rd32(g_cmd + 8);
        c.np = std::min<int>((int)(got - 12) / 4, 5);
        for (int i = 0; i < c.np; i++) c.p[i] = rd32(g_cmd + 12 + 4 * i);
        if (!dispatch(c)) return;
    }
}

static void thread_main(void *) {
    while (!g_quit->load()) {
        set_status("Esperando cable USB...");
        log_write("mtp: esperando cable");
        if (!usb::wait_ready()) break;
        set_status("Cable conectado, esperando al PC...");
        log_write("mtp: cable conectado (paquete max %u)", usb::max_packet());
        rx_reset();
        session_loop();
        usb::cancel_all();
        rx_reset();
        log_write("mtp: sesion USB terminada (cable desconectado o error USB)");
        g_session = false;
        g_send.pending = false;
        saves::unmount_all();
    }
}

static Thread g_thread;

Result start(std::atomic<bool> *quit) {
    g_quit = quit;
    mutexInit(&g_status_mtx);
    g_buf = (u8 *)memalign(0x1000, BUF_SIZE);
    g_buf2 = (u8 *)memalign(0x1000, BUF_SIZE);
    for (int i = 0; i < RX_N; i++) g_rx[i] = (u8 *)memalign(0x1000, RX_SIZE);
    g_cmd = (u8 *)memalign(0x1000, 0x1000);
    if (!g_buf || !g_buf2 || !g_cmd || !g_rx[0] || !g_rx[1] || !g_rx[2]) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    mkdir("sdmc:/switch", 0777);
    Result rc = usb::init(quit);
    if (R_FAILED(rc)) { usb::exit(); return rc; }
    rc = threadCreate(&g_thread, thread_main, nullptr, nullptr, 0x20000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    return rc;
}

bool stop() {
    // Nunca dejar la app colgada al salir: si el hilo USB no responde en 3 s, se
    // sale igual (el sistema libera todo al cerrar la app) y se anota dónde estaba.
    if (R_FAILED(waitSingleHandle(g_thread.handle, 3'000'000'000ULL))) {
        log_write("salida: el hilo USB no termina (ultima llamada: %s); salida forzada", usb::where());
        return false;
    }
    threadClose(&g_thread);
    if (!usb::exit()) return false;
    free(g_buf);
    free(g_buf2);
    for (int i = 0; i < RX_N; i++) free(g_rx[i]);
    free(g_cmd);
    return true;
}

} // namespace mtp
