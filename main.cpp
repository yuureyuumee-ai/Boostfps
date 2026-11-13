// BoostFPS v1.0.0 - mod AML untuk GTA San Andreas Android v2.00 (armeabi-v7a)
//
// AKTIF   : frame limiter (pacing di eglSwapBuffers milik libEGL.so, tidak menyentuh kode game),
//           config INI tervalidasi + dibuat otomatis, logging logcat (tag "BoostFPS"),
//           alat verifikasi simbol libGTASA.so (DumpSymbols=1).
// NONAKTIF: pengangkatan batas FPS bawaan game dan semua opsi grafis (DrawDistance, LODDistance,
//           Shadows, Particles, Grass, Reflections, WaterQuality). Offset/simbolnya belum
//           terverifikasi -> PERLU DIVERIFIKASI. Nilainya tetap dibaca, divalidasi, dan dicatat.
//
// Hanya bergantung pada amlmod.h (MYMOD, ON_MOD_PRELOAD, ON_MOD_LOAD, aml->GetLibHandle, aml->Hook).
// INI dibaca/ditulis sendiri agar nama file pasti BoostFPS.ini dan tiap key punya komentar ';'.

#include <amlmod.h>

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>

MYMOD(com.boostfps.gtasa, BoostFPS, 1.0.0, BoostFPS)

#define BFPS_TAG "BoostFPS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, BFPS_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, BFPS_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, BFPS_TAG, __VA_ARGS__)

namespace {

constexpr const char* kVersion = "1.0.0";
constexpr const char* kPackage = "com.rockstargames.gtasa";
constexpr const char* kIniName = "BoostFPS.ini";
constexpr size_t kMaxIniBytes = 16384;
constexpr int kDumpMax = 600;

// Konvensi AML: <Android/data>/<package>/configs/. Jika AML Anda memakai path lain, ubah di sini.
const char* const kConfigDirs[] = {
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/configs",
    "/sdcard/Android/data/com.rockstargames.gtasa/configs",
};

// Isi ini HARUS identik dengan BoostFPS.ini yang dikirim.
const char* const kDefaultIni = R"INI(; ==========================================================================
; BoostFPS v1.0.0 - AML mod untuk GTA San Andreas Android v2.00 (armeabi-v7a)
; Lokasi file ini : Android/data/com.rockstargames.gtasa/configs/BoostFPS.ini
; Nilai tidak valid/rusak -> dikembalikan ke default. Nilai di luar rentang -> dipotong (clamp).
; Hasil pembacaan dicatat di logcat (tag: BoostFPS). Restart game setelah mengubah file ini.
; ==========================================================================

[BoostFPS]

; --------------------------------------------------------------------------
; FRAME LIMITER
; --------------------------------------------------------------------------
; Fungsi        : menahan laju eglSwapBuffers agar tidak melebihi angka ini.
; Nilai valid   : 0 (mod tidak membatasi) | 30 | 45 | 60
; Default       : 30
; CATATAN 1     : mod ini HANYA menahan/menurunkan FPS. Batas FPS bawaan game TIDAK diangkat
;                 (offset batasnya belum terverifikasi). Jadi 45, 60, atau 0 tidak membuat
;                 game melampaui batas bawaannya.
; CATATAN 2     : fisika dan logika game terikat ke CTimer. Jika batas bawaan suatu saat
;                 terangkat, di atas 30 FPS kendaraan, animasi, dan waktu game bisa berjalan
;                 terlalu cepat. Jika game terasa terlalu cepat, pakai 30.
FPSLimit=30

; --------------------------------------------------------------------------
; OPSI GRAFIS - BELUM AKTIF (PERLU DIVERIFIKASI)
; Hook untuk opsi di bawah belum punya offset/simbol terverifikasi untuk libGTASA.so v2.00,
; sehingga TIDAK diterapkan di build ini. Nilai tetap dibaca, divalidasi, dan dicatat ke log;
; mod mencatat peringatan jika nilainya diubah dari default.
; --------------------------------------------------------------------------

; Fungsi        : skala jarak pandang (draw distance).
; Nilai valid   : 0.25 - 2.00
; Default       : 1.00 (asli)
DrawDistance=1.00

; Fungsi        : skala jarak LOD.
; Nilai valid   : 0.25 - 2.00
; Default       : 1.00 (asli)
LODDistance=1.00

; Fungsi        : bayangan.
; Nilai valid   : 0 = mati | 1 = asli
; Default       : 1 (asli)
Shadows=1

; Fungsi        : partikel / efek.
; Nilai valid   : 0 = mati | 1 = asli
; Default       : 1 (asli)
Particles=1

; Fungsi        : rumput.
; Nilai valid   : 0 = mati | 1 = asli
; Default       : 1 (asli)
Grass=1

; Fungsi        : refleksi.
; Nilai valid   : 0 = mati | 1 = asli
; Default       : 1 (asli)
Reflections=1

; Fungsi        : kualitas air.
; Nilai valid   : 0 = rendah | 1 = sedang | 2 = asli
; Default       : 2 (asli)
WaterQuality=2

; --------------------------------------------------------------------------
; ALAT VERIFIKASI (untuk mencari simbol/offset di libGTASA.so milik Anda)
; --------------------------------------------------------------------------

; Fungsi        : 1 = saat game mulai, tulis simbol libGTASA.so yang cocok dengan DumpFilter
;                 ke logcat (tag BoostFPS), maksimal 600 baris. Hanya membaca, tidak mengubah game.
; Nilai valid   : 0 | 1
; Default       : 0
DumpSymbols=0

; Fungsi        : kata kunci pencarian simbol, dipisah koma, tidak peka huruf besar/kecil.
; Nilai valid   : teks, maksimal 255 karakter, tanpa tanda titik koma
; Default       : timer,fps,rsglobal,shadow,grass,particle,reflect,water,lod,farclip,drawdist
DumpFilter=timer,fps,rsglobal,shadow,grass,particle,reflect,water,lod,farclip,drawdist
)INI";

// ---------------------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------------------

struct Config {
    int FPSLimit = 30;
    float DrawDistance = 1.0f;
    float LODDistance = 1.0f;
    int Shadows = 1;
    int Particles = 1;
    int Grass = 1;
    int Reflections = 1;
    int WaterQuality = 2;
    int DumpSymbols = 0;
    char DumpFilter[256] = "timer,fps,rsglobal,shadow,grass,particle,reflect,water,lod,farclip,drawdist";
};

Config g_cfg;

char* Trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
    *e = '\0';
    return s;
}

bool ParseLong(const char* s, long& out) {
    if (!*s) return false;
    char* end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = v;
    return true;
}

bool ParseFloat(const char* s, float& out) {
    if (!*s) return false;
    char* end = nullptr;
    errno = 0;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

void SetInt(const char* key, const char* val, long lo, long hi, int& dst) {
    long v;
    if (!ParseLong(val, v)) {
        LOGW("%s: nilai rusak '%s' -> memakai %d", key, val, dst);
        return;
    }
    if (v < lo || v > hi) {
        long c = v < lo ? lo : hi;
        LOGW("%s=%ld di luar rentang [%ld..%ld] -> dipotong ke %ld", key, v, lo, hi, c);
        v = c;
    }
    dst = static_cast<int>(v);
}

void SetFloat(const char* key, const char* val, float lo, float hi, float& dst) {
    float v;
    if (!ParseFloat(val, v)) {
        LOGW("%s: nilai rusak '%s' -> memakai %.2f", key, val, dst);
        return;
    }
    if (v < lo || v > hi) {
        float c = v < lo ? lo : hi;
        LOGW("%s=%.3f di luar rentang [%.2f..%.2f] -> dipotong ke %.2f", key, v, lo, hi, c);
        v = c;
    }
    dst = v;
}

void ApplyKV(Config& c, const char* key, const char* val) {
    if (!strcasecmp(key, "FPSLimit")) {
        long v;
        if (ParseLong(val, v) && (v == 0 || v == 30 || v == 45 || v == 60)) {
            c.FPSLimit = static_cast<int>(v);
        } else {
            LOGW("FPSLimit: '%s' tidak valid (hanya 0/30/45/60) -> memakai %d", val, c.FPSLimit);
        }
    } else if (!strcasecmp(key, "DrawDistance")) {
        SetFloat("DrawDistance", val, 0.25f, 2.0f, c.DrawDistance);
    } else if (!strcasecmp(key, "LODDistance")) {
        SetFloat("LODDistance", val, 0.25f, 2.0f, c.LODDistance);
    } else if (!strcasecmp(key, "Shadows")) {
        SetInt("Shadows", val, 0, 1, c.Shadows);
    } else if (!strcasecmp(key, "Particles")) {
        SetInt("Particles", val, 0, 1, c.Particles);
    } else if (!strcasecmp(key, "Grass")) {
        SetInt("Grass", val, 0, 1, c.Grass);
    } else if (!strcasecmp(key, "Reflections")) {
        SetInt("Reflections", val, 0, 1, c.Reflections);
    } else if (!strcasecmp(key, "WaterQuality")) {
        SetInt("WaterQuality", val, 0, 2, c.WaterQuality);
    } else if (!strcasecmp(key, "DumpSymbols")) {
        SetInt("DumpSymbols", val, 0, 1, c.DumpSymbols);
    } else if (!strcasecmp(key, "DumpFilter")) {
        if (*val && strlen(val) < sizeof(c.DumpFilter)) {
            snprintf(c.DumpFilter, sizeof(c.DumpFilter), "%s", val);
        } else {
            LOGW("DumpFilter: kosong atau lebih dari 255 karakter -> memakai default");
        }
    } else {
        LOGW("key tidak dikenal diabaikan: %s", key);
    }
}

// buf dimodifikasi (di-tokenize), harus berakhir NUL.
void ParseIni(char* buf, Config& c) {
    char* save = nullptr;
    for (char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
        line = Trim(line);
        if (!*line || *line == ';' || *line == '#' || *line == '[') continue;
        char* eq = strchr(line, '=');
        if (!eq) {
            LOGW("baris tanpa '=' diabaikan: %s", line);
            continue;
        }
        *eq = '\0';
        char* key = Trim(line);
        char* val = eq + 1;
        char* sc = strchr(val, ';');
        if (sc) *sc = '\0';
        ApplyKV(c, key, Trim(val));
    }
}

void MkdirP(const char* dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

bool WriteDefaultIni(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(kDefaultIni);
    bool ok = fwrite(kDefaultIni, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// true = file ada dan sudah diproses (isi rusak/terlalu besar -> default dipakai).
bool ReadIni(const char* path, Config& c) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGW("tidak bisa membuka %s: %s", path, strerror(errno));
        return false;
    }
    static char buf[kMaxIniBytes + 1];
    size_t n = fread(buf, 1, kMaxIniBytes, f);
    bool tooBig = (fgetc(f) != EOF);
    fclose(f);
    if (tooBig) {
        LOGW("%s lebih dari %u byte -> memakai nilai default", path, static_cast<unsigned>(kMaxIniBytes));
        return true;
    }
    buf[n] = '\0';
    ParseIni(buf, c);
    return true;
}

// Pass 1: pakai file yang sudah ada. Pass 2: buat file default di direktori pertama yang bisa ditulis.
bool LoadConfigFrom(const char* const* dirs, size_t count, Config& c) {
    char path[600];
    for (size_t i = 0; i < count; ++i) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], kIniName);
        if (access(path, R_OK) == 0) {
            LOGI("membaca config: %s", path);
            return ReadIni(path, c);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        MkdirP(dirs[i]);
        snprintf(path, sizeof(path), "%s/%s", dirs[i], kIniName);
        if (WriteDefaultIni(path)) {
            LOGI("config default dibuat: %s", path);
            return ReadIni(path, c);
        }
    }
    LOGW("tidak bisa membaca/membuat %s di direktori mana pun -> memakai nilai default", kIniName);
    return false;
}

void LogConfig(const Config& c) {
    LOGI("config: FPSLimit=%d DrawDistance=%.2f LODDistance=%.2f Shadows=%d Particles=%d Grass=%d "
         "Reflections=%d WaterQuality=%d DumpSymbols=%d",
         c.FPSLimit, c.DrawDistance, c.LODDistance, c.Shadows, c.Particles, c.Grass, c.Reflections,
         c.WaterQuality, c.DumpSymbols);
}

void ReportUnverified(const Config& c) {
    const Config d;
    auto warnF = [](const char* k, float v, float def) {
        if (std::fabs(v - def) > 1e-4f)
            LOGW("%s=%.2f TIDAK diterapkan: hook PERLU DIVERIFIKASI (nonaktif di build ini)", k, v);
    };
    auto warnI = [](const char* k, int v, int def) {
        if (v != def) LOGW("%s=%d TIDAK diterapkan: hook PERLU DIVERIFIKASI (nonaktif di build ini)", k, v);
    };
    warnF("DrawDistance", c.DrawDistance, d.DrawDistance);
    warnF("LODDistance", c.LODDistance, d.LODDistance);
    warnI("Shadows", c.Shadows, d.Shadows);
    warnI("Particles", c.Particles, d.Particles);
    warnI("Grass", c.Grass, d.Grass);
    warnI("Reflections", c.Reflections, d.Reflections);
    warnI("WaterQuality", c.WaterQuality, d.WaterQuality);
    if (c.FPSLimit != 30) {
        LOGW("FPSLimit=%d: mod hanya menahan FPS; batas bawaan game tidak diangkat (PERLU DIVERIFIKASI). "
             "Di atas 30 FPS fisika/CTimer bisa berjalan terlalu cepat jika batas bawaan terangkat.",
             c.FPSLimit);
    }
}

// ---------------------------------------------------------------------------------------------
// Frame limiter (pacing di eglSwapBuffers)
// ---------------------------------------------------------------------------------------------

using SwapFn = unsigned int (*)(void*, void*);  // EGLBoolean (*)(EGLDisplay, EGLSurface)

SwapFn g_origSwap = nullptr;
std::atomic<int64_t> g_targetNs{0};     // 0 = tanpa batas
std::atomic<int64_t> g_lastRelease{0};  // waktu rilis frame sebelumnya (CLOCK_MONOTONIC, ns)

inline int64_t NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void SleepUntil(int64_t due) {
    for (;;) {
        int64_t rem = due - NowNs();
        if (rem <= 0) return;
        timespec ts;
        ts.tv_sec = static_cast<time_t>(rem / 1000000000LL);
        ts.tv_nsec = static_cast<long>(rem % 1000000000LL);
        nanosleep(&ts, nullptr);  // EINTR -> loop menghitung ulang sisa waktu
    }
}

unsigned int Hook_eglSwapBuffers(void* dpy, void* surf) {
    const int64_t target = g_targetNs.load(std::memory_order_relaxed);
    if (target > 0) {
        const int64_t now = NowNs();
        const int64_t last = g_lastRelease.load(std::memory_order_relaxed);
        int64_t release = now;
        if (last != 0) {
            const int64_t due = last + target;
            if (now < due) {
                SleepUntil(due);
                release = due;
            }
            // frame sudah lambat: jangan mengejar ketertinggalan (hindari burst)
        }
        g_lastRelease.store(release, std::memory_order_relaxed);
    }
    SwapFn orig = g_origSwap;
    return orig ? orig(dpy, surf) : 1u;  // 1 = EGL_TRUE; hanya relevan di celah sangat singkat saat hook dipasang
}

bool InstallLimiter(const Config& c) {
    if (c.FPSLimit <= 0) {
        g_targetNs.store(0);
        LOGI("FPSLimit=0: limiter BoostFPS tidak dipasang");
        return true;
    }
    g_targetNs.store(1000000000LL / c.FPSLimit);

    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) {
        LOGE("limiter GAGAL: dlopen libEGL.so: %s", dlerror());
        return false;
    }
    void* sym = dlsym(egl, "eglSwapBuffers");
    if (!sym) {
        LOGE("limiter GAGAL: simbol eglSwapBuffers tidak ditemukan di libEGL.so");
        return false;
    }
    aml->Hook(sym, reinterpret_cast<void*>(Hook_eglSwapBuffers), reinterpret_cast<void**>(&g_origSwap));
    if (!g_origSwap) {
        LOGE("limiter GAGAL: aml->Hook tidak mengisi pointer fungsi asli");
        return false;
    }
    LOGI("limiter OK: eglSwapBuffers di-hook, target %d FPS (%lld ns/frame)", c.FPSLimit,
         static_cast<long long>(g_targetNs.load()));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Alat verifikasi: dump simbol ELF32 ARM dari file libGTASA.so (hanya membaca)
// ---------------------------------------------------------------------------------------------

struct Ehdr32 {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Shdr32 {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
};
struct Sym32 {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
};
static_assert(sizeof(Ehdr32) == 52, "Ehdr32");
static_assert(sizeof(Shdr32) == 40, "Shdr32");
static_assert(sizeof(Sym32) == 16, "Sym32");

bool ContainsI(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return false;
    for (; *hay; ++hay) {
        if (strncasecmp(hay, needle, nl) == 0) return true;
    }
    return false;
}

// Mengembalikan jumlah simbol yang dicatat, atau -1 jika gagal.
int DumpFromImage(const uint8_t* m, size_t size, const char* filter) {
    if (size < sizeof(Ehdr32)) {
        LOGE("dump: file terlalu kecil");
        return -1;
    }
    Ehdr32 eh;
    memcpy(&eh, m, sizeof(eh));
    if (!(m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') || eh.e_ident[4] != 1 ||
        eh.e_machine != 40) {
        LOGE("dump: bukan ELF32 ARM (mungkin libGTASA.so dimuat langsung dari APK)");
        return -1;
    }
    if (eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize != sizeof(Shdr32) ||
        static_cast<uint64_t>(eh.e_shoff) + static_cast<uint64_t>(eh.e_shnum) * sizeof(Shdr32) > size) {
        LOGE("dump: section header tidak ada/rusak (file di-strip?)");
        return -1;
    }

    char fbuf[256];
    snprintf(fbuf, sizeof(fbuf), "%s", filter);
    const char* toks[32];
    int nt = 0;
    char* save = nullptr;
    for (char* t = strtok_r(fbuf, ",", &save); t && nt < 32; t = strtok_r(nullptr, ",", &save)) {
        t = Trim(t);
        if (*t) toks[nt++] = t;
    }
    if (nt == 0) {
        LOGW("dump: DumpFilter kosong, tidak ada yang dicari");
        return 0;
    }

    int matched = 0;
    bool full = false;
    for (uint32_t i = 0; i < eh.e_shnum && !full; ++i) {
        Shdr32 sh;
        memcpy(&sh, m + eh.e_shoff + static_cast<size_t>(i) * sizeof(Shdr32), sizeof(sh));
        if (sh.sh_type != 2 && sh.sh_type != 11) continue;  // SHT_SYMTAB / SHT_DYNSYM
        if (sh.sh_link >= eh.e_shnum) continue;
        Shdr32 st;
        memcpy(&st, m + eh.e_shoff + static_cast<size_t>(sh.sh_link) * sizeof(Shdr32), sizeof(st));
        if (static_cast<uint64_t>(sh.sh_offset) + sh.sh_size > size) continue;
        if (static_cast<uint64_t>(st.sh_offset) + st.sh_size > size || st.sh_size == 0) continue;
        const char* strtab = reinterpret_cast<const char*>(m + st.sh_offset);
        const uint32_t strsz = st.sh_size;
        const char* kind = (sh.sh_type == 11) ? "dynsym" : "symtab";
        const uint32_t cnt = sh.sh_size / sizeof(Sym32);
        for (uint32_t k = 1; k < cnt; ++k) {
            Sym32 s;
            memcpy(&s, m + sh.sh_offset + static_cast<size_t>(k) * sizeof(Sym32), sizeof(s));
            const uint8_t type = s.st_info & 0x0f;  // 1 = OBJECT, 2 = FUNC
            if (s.st_shndx == 0 || s.st_value == 0 || (type != 1 && type != 2)) continue;
            if (s.st_name >= strsz) continue;
            const size_t maxlen = strsz - s.st_name;
            const char* name = strtab + s.st_name;
            if (strnlen(name, maxlen) >= maxlen) continue;
            bool hit = false;
            for (int j = 0; j < nt && !hit; ++j) hit = ContainsI(name, toks[j]);
            if (!hit) continue;
            if (matched >= kDumpMax) {
                full = true;
                break;
            }
            ++matched;
            LOGI("sym[%s] %s val=0x%08x size=%u %s", kind, name, s.st_value, s.st_size,
                 type == 2 ? ((s.st_value & 1u) ? "FUNC-thumb" : "FUNC-arm") : "OBJECT");
        }
    }
    if (full) LOGW("dump: dipotong pada %d simbol; persempit DumpFilter", kDumpMax);
    return matched;
}

int DumpFile(const char* path, const char* filter) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("dump: open %s gagal: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        LOGE("dump: fstat gagal");
        close(fd);
        return -1;
    }
    const size_t size = static_cast<size_t>(st.st_size);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        LOGE("dump: mmap gagal: %s", strerror(errno));
        return -1;
    }
    int r = DumpFromImage(static_cast<const uint8_t*>(map), size, filter);
    munmap(map, size);
    return r;
}

// Cari path file libGTASA.so dan alamat basenya dari /proc/self/maps.
bool FindModule(const char* suffix, char* path, size_t cap, uintptr_t* base) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, suffix)) continue;
        char* p = strchr(line, '/');
        if (!p) continue;
        size_t l = strlen(p);
        while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = '\0';
        snprintf(path, cap, "%s", p);
        *base = static_cast<uintptr_t>(strtoull(line, nullptr, 16));
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}

void DumpSymbols(const Config& c) {
    char path[512];
    uintptr_t base = 0;
    if (!FindModule("/libGTASA.so", path, sizeof(path), &base)) {
        LOGE("dump: libGTASA.so tidak ada di /proc/self/maps");
        return;
    }
    LOGI("dump: libGTASA.so path=%s base=0x%lx (alamat runtime = base + val, bit0 thumb dibuang)", path,
         static_cast<unsigned long>(base));
    if (strstr(path, ".apk")) {
        LOGE("dump: libGTASA.so dimuat dari APK, tidak bisa dibaca sebagai file ELF mandiri");
        return;
    }
    int n = DumpFile(path, c.DumpFilter);
    LOGI("dump: selesai, %d simbol cocok", n);
}

bool ProcessIsGtaSa() {
    char buf[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return true;  // tidak bisa dipastikan -> jangan blokir
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, kPackage) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Entry point AML
// ---------------------------------------------------------------------------------------------

ON_MOD_PRELOAD() {
    LOGI("BoostFPS v%s dimuat (preload)", kVersion);
    LoadConfigFrom(kConfigDirs, sizeof(kConfigDirs) / sizeof(kConfigDirs[0]), g_cfg);
    LogConfig(g_cfg);
}

ON_MOD_LOAD() {
    LOGI("BoostFPS v%s: OnModLoad", kVersion);
    if (!ProcessIsGtaSa()) {
        LOGW("proses bukan %s; mod ditargetkan hanya untuk GTA SA v2.00, lanjut dengan risiko sendiri", kPackage);
    }
    void* hGTASA = aml->GetLibHandle("libGTASA.so");
    if (!hGTASA) {
        LOGE("libGTASA.so belum termuat / tidak ditemukan -> semua patch dibatalkan (tanpa crash)");
        return;
    }
    LOGI("libGTASA.so termuat (handle=%p)", hGTASA);

    if (g_cfg.DumpSymbols) DumpSymbols(g_cfg);
    ReportUnverified(g_cfg);

    if (!InstallLimiter(g_cfg)) {
        LOGW("frame limiter tidak aktif; game berjalan seperti aslinya");
    }
    LOGI("BoostFPS siap");
}
