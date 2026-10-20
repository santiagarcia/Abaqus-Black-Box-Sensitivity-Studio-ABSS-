/* ABSS ELF host: runs a Linux x86-64 material library (.so) natively on Windows, without WSL, for the material bridge.
 *
 * It maps the shared object itself (PT_LOAD segments, RELA relocations, init arrays) and binds its imports to
 * Windows implementations called through System V ABI thunks (GCC's sysv_abi attribute): the C runtime and math
 * functions, LAPACK (OpenBLAS), and the parts of the gfortran 8+ runtime that material models use (INQUIRE/OPEN of
 * optional input files, list-directed WRITE of messages, string and array helpers). Anything it cannot reproduce
 * exactly - a Fortran READ, thread-local storage, an unknown import - stops with a message naming it, so the
 * library is never run with a silent approximation. Then it speaks the bridge protocol of abss_matbridge.py on
 * stdin/stdout (see src/oracle/abaqus_matbridge.cpp).
 *
 *   abss_elfhost --lib x.so --abi regular --step ti_regular [--init ti_reg_sdvini] [--ntens 6 --nstatv 132 --nprops 95]
 *   abss_elfhost --lib x.so --abi umat --umat umat_ [--sdvini sdvini_]
 *   ... --selftest                       load, relocate, resolve the entry points, report
 *   ... --uniaxial props.txt --increments 12 --deps 1.25e-3     drive the F path of the vendor example (verification)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("ABSS ELF host: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    exit(3);
}

/* ------------------------------------------------------------------ ELF definitions (64-bit, little endian) --- */
typedef struct { unsigned char ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; } Shdr;
typedef struct { uint32_t name; unsigned char info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;
typedef struct { int64_t tag; uint64_t val; } Dyn;

static unsigned char* g_file;
static size_t g_fileSize;
static unsigned char* g_base;
static const Sym* g_dynsym;
static const char* g_dynstr;
static size_t g_nsym;

/* ------------------------------------------------------------------------------- C runtime and math thunks --- */
static SYSV void* t_malloc(size_t n) { return malloc(n); }
static SYSV void t_free(void* p) { free(p); }
static SYSV void* t_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static SYSV void* t_memset(void* d, int c, size_t n) { return memset(d, c, n); }
static SYSV double t_exp(double x) { return exp(x); }
static SYSV double t_log(double x) { return log(x); }
static SYSV double t_pow(double x, double y) { return pow(x, y); }
static SYSV long long t_lround(double x) { return llround(x); } /* Linux 'long' is 64-bit */
static SYSV void t_cxa_finalize(void* d) { (void)d; }

/* -------------------------------------------------------------------------------------- LAPACK (OpenBLAS) --- */
extern void dgetrf_(const int*, const int*, double*, const int*, int*, int*);
extern void dgetrs_(const char*, const int*, const int*, const double*, const int*, const int*, double*, const int*, int*, size_t);
extern void dspevd_(const char*, const char*, const int*, double*, double*, double*, const int*, double*, const int*, int*,
                    const int*, int*, size_t, size_t);
static SYSV void t_dgetrf(const int* m, const int* n, double* a, const int* lda, int* ipiv, int* info) { dgetrf_(m, n, a, lda, ipiv, info); }
static SYSV void t_dgetrs(const char* tr, const int* n, const int* nrhs, const double* a, const int* lda, const int* ipiv, double* b,
                          const int* ldb, int* info, size_t trlen) { dgetrs_(tr, n, nrhs, a, lda, ipiv, b, ldb, info, trlen); }
static SYSV void t_dspevd(const char* jobz, const char* uplo, const int* n, double* ap, double* w, double* z, const int* ldz,
                          double* work, const int* lwork, int* iwork, const int* liwork, int* info, size_t l1, size_t l2) {
    dspevd_(jobz, uplo, n, ap, w, z, ldz, work, lwork, iwork, liwork, info, l1, l2);
}

/* ------------------------------------------------------------------------------ gfortran runtime (GCC >= 8) --- */
/* libgfortran io.h (GCC >= 8, gfc_charlen_type = size_t): common header of every I/O parameter block. */
typedef struct { int32_t flags, unit; const char* filename; int32_t line; size_t iomsg_len; char* iomsg; int32_t* iostat; } IoCommon;
enum { IOP_LIBRETURN_MASK = 3, IOP_LIBRETURN_ERROR = 1, IOP_ERR = 1 << 2, IOP_HAS_IOSTAT = 1 << 5, IOP_HAS_IOMSG = 1 << 6 };
/* st_parameter_inquire: common, then pointers to the requested results (flag bits from 1 << 7 upwards). Only these
 * leading fields are used; the FILE= string further on is not read (its position is not relied upon). */
typedef struct { IoCommon common; int32_t *exist, *opened, *number, *named; } IoInquire;
enum { INQ_HAS_EXIST = 1 << 7, INQ_HAS_OPENED = 1 << 8, INQ_HAS_NUMBER = 1 << 9, INQ_HAS_NAMED = 1 << 10 };

static char g_line[4096];
static size_t g_lineLen;

static void fortranName(const char* s, size_t n, char* out, size_t cap) {
    while (n && s[n - 1] == ' ') --n;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}
static void ioError(IoCommon* c, int code, const char* what) {
    if (c->flags & IOP_HAS_IOSTAT) {
        if (c->iostat) *c->iostat = code;
        c->flags = (c->flags & ~IOP_LIBRETURN_MASK) | IOP_LIBRETURN_ERROR;
        return;
    }
    if (c->flags & IOP_ERR) {
        c->flags = (c->flags & ~IOP_LIBRETURN_MASK) | IOP_LIBRETURN_ERROR;
        return;
    }
    die("%s (Fortran I/O at %s:%d without IOSTAT= or ERR=)", what, c->filename ? c->filename : "?", c->line);
}
/* The ELF host has no file system for the library: every INQUIRE answers "does not exist" and every OPEN fails with
 * IOSTAT, and each answer is logged, so a library that relies on input files is visible in abss_bridge.log (run
 * such a library under WSL instead). Optional files with built-in defaults - the usual case - work unchanged. */
static SYSV void g_st_inquire(IoInquire* p) {
    fprintf(stderr, "ABSS ELF host: INQUIRE (%s:%d) answered 'file does not exist' (input files are not read)\n",
            p->common.filename ? p->common.filename : "?", p->common.line);
    if ((p->common.flags & INQ_HAS_EXIST) && p->exist) *p->exist = 0;
    if ((p->common.flags & INQ_HAS_OPENED) && p->opened) *p->opened = 0;
    if ((p->common.flags & INQ_HAS_NUMBER) && p->number) *p->number = -1;
    if ((p->common.flags & INQ_HAS_NAMED) && p->named) *p->named = 0;
    if ((p->common.flags & IOP_HAS_IOSTAT) && p->common.iostat) *p->common.iostat = 0;
}
static SYSV void g_st_open(IoCommon* p) {
    fprintf(stderr, "ABSS ELF host: OPEN (%s:%d) failed with IOSTAT (input files are not read)\n", p->filename ? p->filename : "?", p->line);
    ioError(p, 2, "OPEN of an input file");
}
static SYSV void g_st_close(IoCommon* p) { if (p->flags & IOP_HAS_IOSTAT && p->iostat) *p->iostat = 0; }
static SYSV void g_st_read(IoCommon* p) { die("the library performs a Fortran READ (%s:%d); the ELF host does not support reads - run it under WSL", p->filename ? p->filename : "?", p->line); }
static SYSV void g_st_read_done(void* p) { (void)p; }
static SYSV void g_transfer_read(void* p, void* d, int k) { (void)p, (void)d, (void)k; die("Fortran READ transfer"); }
static SYSV void g_transfer_array_read(void* p, void* d, int k, size_t l) { (void)p, (void)d, (void)k, (void)l; die("Fortran READ transfer"); }
/* WRITE: messages only (list-directed); collected per statement and sent to stderr (the job's abss_bridge.log). */
static SYSV void g_st_write(IoCommon* p) { (void)p; g_lineLen = 0; }
static void append(const char* s, size_t n) {
    if (g_lineLen + n + 2 >= sizeof g_line) n = sizeof g_line - g_lineLen - 2;
    memcpy(g_line + g_lineLen, s, n);
    g_lineLen += n;
}
static SYSV void g_transfer_character_write(void* p, const char* s, size_t n) { (void)p; append(" ", 1); append(s, n); }
static SYSV void g_transfer_integer_write(void* p, const void* v, int kind) {
    (void)p;
    char b[32];
    long long x = kind == 8 ? *(const long long*)v : kind == 2 ? *(const short*)v : kind == 1 ? *(const signed char*)v : *(const int*)v;
    int n = snprintf(b, sizeof b, " %lld", x);
    append(b, (size_t)n);
}
static SYSV void g_transfer_array_write(void* p, void* d, int k, size_t l) { (void)p, (void)d, (void)k, (void)l; append(" [array]", 8); }
static SYSV void g_st_write_done(void* p) {
    (void)p;
    g_line[g_lineLen] = 0;
    fprintf(stderr, "%s\n", g_line);
    fflush(stderr);
}
static SYSV void g_concat_string(size_t destlen, char* dest, size_t l1, const char* s1, size_t l2, const char* s2) {
    if (l1 >= destlen) { memcpy(dest, s1, destlen); return; }
    memcpy(dest, s1, l1);
    size_t r = destlen - l1;
    if (l2 >= r) { memcpy(dest + l1, s2, r); return; }
    memcpy(dest + l1, s2, l2);
    memset(dest + l1 + l2, ' ', r - l2);
}
static SYSV size_t g_string_len_trim(size_t n, const char* s) { while (n && s[n - 1] == ' ') --n; return n; }
static SYSV void g_stop_string(const char* s, size_t n, int quiet) {
    (void)quiet;
    char b[512];
    fortranName(s ? s : "", s ? n : 0, b, sizeof b);
    die("the library executed STOP %s", b);
}
static SYSV void g_os_error(const char* m) { die("library runtime error: %s", m ? m : "?"); }

/* GCC >= 8 array descriptor. */
typedef struct { size_t elem_len; int32_t version; signed char rank, type; int16_t attribute; } DType;
typedef struct { int64_t stride, lbound, ubound; } Dim;
typedef struct { void* base; size_t offset; DType dtype; int64_t span; Dim dim[7]; } Desc;

static int64_t extent(const Desc* d, int i) { int64_t e = d->dim[i].ubound - d->dim[i].lbound + 1; return e > 0 ? e : 0; }
static int64_t count(const Desc* d) { int64_t n = 1; for (int i = 0; i < d->dtype.rank; ++i) n *= extent(d, i); return n; }
static int contiguous(const Desc* d) {
    int64_t s = 1;
    for (int i = 0; i < d->dtype.rank; ++i) { if (extent(d, i) > 1 && d->dim[i].stride != s) return 0; s *= extent(d, i); }
    return 1;
}
/* Element k (column-major over the descriptor's index space) -> address. Strides count in units of 'span' bytes. */
static char* elemAddr(const Desc* d, int64_t k) {
    int64_t off = 0;
    for (int i = 0; i < d->dtype.rank; ++i) { int64_t e = extent(d, i); off += (k % e) * d->dim[i].stride; k /= e; }
    return (char*)d->base + off * (d->span ? d->span : (int64_t)d->dtype.elem_len);
}
static SYSV void* g_internal_pack(Desc* d) {
    if (!d->base || contiguous(d)) return d->base;
    const int64_t n = count(d);
    const size_t es = d->dtype.elem_len;
    char* buf = (char*)malloc((size_t)(n > 0 ? n : 1) * es);
    for (int64_t k = 0; k < n; ++k) memcpy(buf + k * (int64_t)es, elemAddr(d, k), es);
    return buf;
}
static SYSV void g_internal_unpack(Desc* d, const void* data) {
    if (!data || data == d->base) return;
    const int64_t n = count(d);
    const size_t es = d->dtype.elem_len;
    for (int64_t k = 0; k < n; ++k) memcpy(elemAddr(d, k), (const char*)data + k * (int64_t)es, es);
}
static SYSV void g_reshape_r8(Desc* ret, const Desc* src, const Desc* shape, const Desc* pad, const Desc* order) {
    if (pad || order) die("RESHAPE with PAD= or ORDER= is not supported by the ELF host - run the library under WSL");
    const int rank = (int)extent(shape, 0);
    if (!ret->base) {
        int64_t s = 1;
        for (int i = 0; i < rank; ++i) {
            const int64_t e = *(const int64_t*)elemAddr(shape, i);
            ret->dim[i].lbound = 0, ret->dim[i].ubound = e - 1, ret->dim[i].stride = s;
            s *= e;
        }
        ret->offset = 0;
        ret->dtype.elem_len = 8, ret->dtype.rank = (signed char)rank, ret->span = 8;
        ret->base = malloc((size_t)(s > 0 ? s : 1) * 8);
    }
    const int64_t n = count(ret);
    if (count(src) < n) die("RESHAPE: source smaller than the result and no PAD=");
    for (int64_t k = 0; k < n; ++k) *(double*)elemAddr(ret, k) = *(const double*)elemAddr(src, k);
}

typedef struct { const char* name; void* fn; } Import;
static const Import kImports[] = {
    {"malloc", (void*)t_malloc}, {"free", (void*)t_free}, {"memcpy", (void*)t_memcpy}, {"memset", (void*)t_memset},
    {"exp", (void*)t_exp}, {"log", (void*)t_log}, {"pow", (void*)t_pow}, {"lround", (void*)t_lround},
    {"__cxa_finalize", (void*)t_cxa_finalize},
    {"dgetrf_", (void*)t_dgetrf}, {"dgetrs_", (void*)t_dgetrs}, {"dspevd_", (void*)t_dspevd},
    {"_gfortran_st_inquire", (void*)g_st_inquire}, {"_gfortran_st_open", (void*)g_st_open},
    {"_gfortran_st_close", (void*)g_st_close}, {"_gfortran_st_read", (void*)g_st_read},
    {"_gfortran_st_read_done", (void*)g_st_read_done}, {"_gfortran_transfer_integer", (void*)g_transfer_read},
    {"_gfortran_transfer_real", (void*)g_transfer_read}, {"_gfortran_transfer_array", (void*)g_transfer_array_read},
    {"_gfortran_st_write", (void*)g_st_write}, {"_gfortran_st_write_done", (void*)g_st_write_done},
    {"_gfortran_transfer_character_write", (void*)g_transfer_character_write},
    {"_gfortran_transfer_integer_write", (void*)g_transfer_integer_write},
    {"_gfortran_transfer_array_write", (void*)g_transfer_array_write},
    {"_gfortran_concat_string", (void*)g_concat_string}, {"_gfortran_string_len_trim", (void*)g_string_len_trim},
    {"_gfortran_stop_string", (void*)g_stop_string}, {"_gfortran_os_error", (void*)g_os_error},
    {"_gfortran_internal_pack", (void*)g_internal_pack}, {"_gfortran_internal_unpack", (void*)g_internal_unpack},
    {"_gfortran_reshape_r8", (void*)g_reshape_r8},
};

/* --------------------------------------------------------------------------------------------- the loader --- */
static void* importAddr(const char* name, int weak) {
    for (size_t i = 0; i < sizeof kImports / sizeof kImports[0]; ++i)
        if (!strcmp(kImports[i].name, name)) return kImports[i].fn;
    if (weak) return NULL; /* e.g. __gmon_start__, _ITM_* (profiling / transactional memory hooks) */
    die("the library needs '%s', which the ELF host does not provide - run it under WSL", name);
    return NULL;
}

static void loadElf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    g_fileSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (unsigned char*)malloc(g_fileSize);
    if (fread(g_file, 1, g_fileSize, f) != g_fileSize) die("cannot read %s", path);
    fclose(f);
    const Ehdr* eh = (const Ehdr*)g_file;
    if (g_fileSize < sizeof(Ehdr) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1)
        die("%s is not a 64-bit little-endian ELF file", path);
    if (eh->machine != 62 || eh->type != 3) die("%s is not an x86-64 shared object", path);
    const Phdr* ph = (const Phdr*)(g_file + eh->phoff);
    uint64_t hi = 0;
    const Dyn* dyn = NULL;
    for (int i = 0; i < eh->phnum; ++i) {
        if (ph[i].type == 1 && ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        if (ph[i].type == 7) die("the library uses thread-local storage, which the ELF host does not support - run it under WSL");
    }
    g_base = (unsigned char*)VirtualAlloc(NULL, (size_t)hi, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_base) die("cannot reserve %llu bytes for the library image", (unsigned long long)hi);
    for (int i = 0; i < eh->phnum; ++i) {
        if (ph[i].type == 1) memcpy(g_base + ph[i].vaddr, g_file + ph[i].offset, (size_t)ph[i].filesz);  /* rest is zero */
        if (ph[i].type == 2) dyn = (const Dyn*)(g_base + ph[i].vaddr);
    }
    if (!dyn) die("no dynamic section");
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, init = 0, initArr = 0, initArrSz = 0, symtab = 0, strtab = 0;
    for (const Dyn* d = dyn; d->tag; ++d) switch (d->tag) {
            case 7: rela = d->val; break;
            case 8: relasz = d->val; break;
            case 23: jmprel = d->val; break;
            case 2: pltrelsz = d->val; break;
            case 12: init = d->val; break;
            case 25: initArr = d->val; break;
            case 27: initArrSz = d->val; break;
            case 6: symtab = d->val; break;
            case 5: strtab = d->val; break;
            default: break;
        }
    g_dynsym = (const Sym*)(g_base + symtab);
    g_dynstr = (const char*)(g_base + strtab);
    /* Symbol count from the section headers (.dynsym size). */
    const Shdr* sh = (const Shdr*)(g_file + eh->shoff);
    for (int i = 0; i < eh->shnum; ++i)
        if (sh[i].type == 11) g_nsym = (size_t)(sh[i].size / sizeof(Sym));
    const Rela* lists[2] = {(const Rela*)(g_base + rela), (const Rela*)(g_base + jmprel)};
    const uint64_t sizes[2] = {rela ? relasz : 0, jmprel ? pltrelsz : 0};
    for (int l = 0; l < 2; ++l)
        for (uint64_t k = 0; k < sizes[l] / sizeof(Rela); ++k) {
            const Rela* r = &lists[l][k];
            const uint32_t type = (uint32_t)r->info, si = (uint32_t)(r->info >> 32);
            uint64_t* where = (uint64_t*)(g_base + r->offset);
            uint64_t S = 0;
            if (si) {
                const Sym* s = &g_dynsym[si];
                if (s->shndx) S = (uint64_t)(g_base + s->value);
                else S = (uint64_t)importAddr(g_dynstr + s->name, (s->info >> 4) == 2);
            }
            switch (type) {
                case 8: *where = (uint64_t)g_base + (uint64_t)r->addend; break;  /* RELATIVE */
                case 1: *where = S + (uint64_t)r->addend; break;                  /* 64 */
                case 6: case 7: *where = S; break;                                /* GLOB_DAT, JUMP_SLOT */
                case 0: break;
                default: die("unsupported relocation type %u - run the library under WSL", type);
            }
        }
    typedef SYSV void (*InitFn)(int, char**, char**);
    if (init) ((InitFn)(g_base + init))(0, NULL, NULL);
    for (uint64_t k = 0; k < initArrSz / 8; ++k) {
        const uint64_t fn = ((const uint64_t*)(g_base + initArr))[k];
        if (fn && fn != (uint64_t)-1) ((InitFn)fn)(0, NULL, NULL);
    }
}

static void* findExport(const char* name) {
    for (size_t i = 0; i < g_nsym; ++i)
        if (g_dynsym[i].shndx && g_dynsym[i].name && !strcmp(g_dynstr + g_dynsym[i].name, name) && (g_dynsym[i].info & 0xf) == 2)
            return g_base + g_dynsym[i].value;
    return NULL;
}

/* ------------------------------------------------------------------------------------- the bridge protocol --- */
typedef SYSV void (*RegularFn)(int, int, int, int, const double*, double*, double*, double*, const double*, const double*,
                               double, double, double, double, const double*, const double*, const int*, const int*, int, int, double*);
typedef SYSV void (*RegInitFn)(int, int, double*, const double*, const int*, const int*);
typedef SYSV void (*UmatFn)(double*, double*, double*, double*, double*, double*, double*, double*, double*, double*, double*,
                            double*, double*, double*, double*, double*, double*, double*, char*, int*, int*, int*, int*, double*,
                            int*, double*, double*, double*, double*, double*, double*, int*, int*, int*, int*, int*, int*, size_t);
typedef SYSV void (*SdviniFn)(double*, double*, int*, int*, int*, int*, int*, int*);

static HANDLE g_in, g_out;
static int rd(void* p, DWORD n) {
    char* c = (char*)p;
    while (n) { DWORD r = 0; if (!ReadFile(g_in, c, n, &r, NULL) || !r) return 0; c += r; n -= r; }
    return 1;
}
static void wr(const void* p, DWORD n) {
    const char* c = (const char*)p;
    while (n) { DWORD w = 0; if (!WriteFile(g_out, c, n, &w, NULL) || !w) exit(4); c += w; n -= w; }
}
static void fail(const char* msg) {
    int h[2] = {1, (int)strlen(msg)};
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    wr(h, 8);
    wr(msg, (DWORD)h[1]);
}
static const char* opt(int argc, char** argv, const char* key) {
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], key)) return argv[i + 1];
    return NULL;
}
static int flag(int argc, char** argv, const char* key) { for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], key)) return 1; return 0; }

static int uniaxial(RegularFn step, RegInitFn init, const char* propsPath, int incs, double deps) {
    double props[95] = {0};
    FILE* f = fopen(propsPath, "r");
    if (!f) die("cannot open %s", propsPath);
    int n = 0;
    char line[512];
    while (n < 95 && fgets(line, sizeof line, f)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char* t = strtok(line, " ,\t\r\n"); t && n < 95; t = strtok(NULL, " ,\t\r\n")) props[n++] = atof(t);
    }
    fclose(f);
    if (n != 95) die("%s: expected 95 constants, found %d", propsPath, n);
    double stress[6] = {0}, statev[132] = {0}, dd[36], f0[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, f1[9], coords[3] = {0}, celent = 1, pn;
    int noel = 1, npt = 1;
    init(1, 132, statev, coords, &noel, &npt);
    for (int k = 1; k <= incs; ++k) {
        const double e = k * deps;
        memset(f1, 0, sizeof f1);
        f1[0] = 1 + 0.35 * e, f1[4] = 1 - 0.20 * e, f1[8] = 1 + e;  /* diagonal: Fortran order equals transpose */
        pn = 0;
        step(1, 95, 6, 132, props, stress, statev, dd, f0, f1, 1.0, (double)(k - 1), 0, 0, coords, &celent, &noel, &npt, 1, k, &pn);
        if (pn < 1) { printf("increment %d rejected (PNEWDT %g)\n", k, pn); return 1; }
        int bad = 0, first = -1;
        for (int q = 0; q < 132; ++q) if (!isfinite(statev[q])) { ++bad; if (first < 0) first = q; }
        int badD = 0;
        for (int q = 0; q < 36; ++q) badD += !isfinite(dd[q]);
        if (bad || badD) printf("increment %d: %d non-finite state variable(s) (first SDV%d), %d non-finite tangent entries\n", k, bad, first + 1, badD);
        memcpy(f0, f1, sizeof f0);
    }
    printf("stress %.17g %.17g %.17g %.17g %.17g %.17g\n",stress[0], stress[1], stress[2], stress[3], stress[4], stress[5]);
    return 0;
}

int main(int argc, char** argv) {
    const char* lib = opt(argc, argv, "--lib");
    if (!lib) die("usage: abss_elfhost --lib x.so --abi regular|umat ...");
    _setmode(_fileno(stdout), _O_BINARY);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    loadElf(lib);
    const char* abi = opt(argc, argv, "--abi");
    const int regular = abi && !strcmp(abi, "regular");
    void* stepP = findExport(opt(argc, argv, regular ? "--step" : "--umat") ? opt(argc, argv, regular ? "--step" : "--umat") : "umat_");
    const char* initName = opt(argc, argv, regular ? "--init" : "--sdvini");
    void* initP = initName ? findExport(initName) : NULL;
    if (!stepP) die("entry point not found in %s", lib);
    if (initName && !initP) die("state initialiser %s not found in %s", initName, lib);
    const int needNtens = opt(argc, argv, "--ntens") ? atoi(opt(argc, argv, "--ntens")) : 0;
    const int needNstatv = opt(argc, argv, "--nstatv") ? atoi(opt(argc, argv, "--nstatv")) : 0;
    const int needNprops = opt(argc, argv, "--nprops") ? atoi(opt(argc, argv, "--nprops")) : 0;
    if (flag(argc, argv, "--selftest")) {
        printf("ABSS-SELFTEST OK abi=%s init=%s host=native-elf\n", regular ? "regular" : "umat", initP ? "yes" : "no");
        return 0;
    }
    if (opt(argc, argv, "--uniaxial")) {
        if (!regular || !initP) die("--uniaxial needs --abi regular with --init");
        const char* ni = opt(argc, argv, "--increments");
        const char* de = opt(argc, argv, "--deps");
        return uniaxial((RegularFn)stepP, (RegInitFn)initP, opt(argc, argv, "--uniaxial"), ni ? atoi(ni) : 12, de ? atof(de) : 1.25e-3);
    }
    wr("ABSSMB01", 8);
    for (;;) {
        int op;
        if (!rd(&op, 4)) return 0;
        if (op == 1) {
            int iv[6];
            double c[3];
            if (!rd(iv, sizeof iv) || !rd(c, sizeof c)) return 0;
            double* sv = (double*)calloc((size_t)(iv[0] > 0 ? iv[0] : 1), 8);
            if (!initP) { fail("Abaqus called SDVINI but the material library has no state initialiser"); free(sv); continue; }
            if (regular) ((RegInitFn)initP)(1, iv[0], sv, c, &iv[2], &iv[3]);
            else ((SdviniFn)initP)(sv, c, &iv[0], &iv[1], &iv[2], &iv[3], &iv[4], &iv[5]);
            int ok = 1;
            for (int k = 0; k < iv[0]; ++k) ok = ok && isfinite(sv[k]);
            if (!ok) { fail("the state initialiser returned non-finite values"); free(sv); continue; }
            int zero = 0;
            wr(&zero, 4);
            wr(sv, (DWORD)(8 * iv[0]));
            free(sv);
        } else if (op == 2) {
            int iv[11];
            double dv[10];
            if (!rd(iv, sizeof iv) || !rd(dv, sizeof dv)) return 0;
            const int ntens = iv[2], nstatv = iv[3], nprops = iv[4];
            double* stress = (double*)calloc((size_t)ntens, 8);
            double* statev = (double*)calloc((size_t)(nstatv > 0 ? nstatv : 1), 8);
            double* stran = (double*)calloc((size_t)ntens, 8);
            double* dstran = (double*)calloc((size_t)ntens, 8);
            double* props = (double*)calloc((size_t)(nprops > 0 ? nprops : 1), 8);
            double* ddsdde = (double*)calloc((size_t)(ntens * ntens), 8);
            double* ddsddt = (double*)calloc((size_t)ntens, 8);
            double* drplde = (double*)calloc((size_t)ntens, 8);
            double coords[3], drot[9], f0[9], f1[9];
            char cmname[81] = {0};
            if (!rd(stress, 8 * ntens) || !rd(statev, 8 * nstatv) || !rd(stran, 8 * ntens) || !rd(dstran, 8 * ntens) ||
                !rd(props, 8 * nprops) || !rd(coords, 24) || !rd(drot, 72) || !rd(f0, 72) || !rd(f1, 72) || !rd(cmname, 80))
                return 0;
            char why[256] = "";
            if (needNtens && ntens != needNtens) snprintf(why, sizeof why, "element %d: NTENS = %d (the library requires %d)", iv[5], ntens, needNtens);
            else if (needNstatv && nstatv != needNstatv) snprintf(why, sizeof why, "element %d: NSTATV = %d (the library requires %d)", iv[5], nstatv, needNstatv);
            else if (needNprops && nprops != needNprops) snprintf(why, sizeof why, "element %d: NPROPS = %d (the library requires %d)", iv[5], nprops, needNprops);
            double sse = dv[0], spd = dv[1], scd = dv[2], rpl = 0, drpldt = 0, pnew = dv[9];
            if (!why[0]) {
                if (regular) {
                    double pn = 1.0, celent = dv[8];
                    int noel = iv[5], npt = iv[6];
                    ((RegularFn)stepP)(1, nprops, ntens, nstatv, props, stress, statev, ddsdde, f0, f1, dv[5], dv[3], dv[6], dv[7],
                                       coords, &celent, &noel, &npt, iv[9], iv[10], &pn);
                    if (pn < pnew) pnew = pn;
                } else {
                    double tm[2] = {dv[3], dv[4]}, dtime = dv[5], temp = dv[6], dtemp = dv[7], celent = dv[8];
                    double predef[64] = {0}, dpred[64] = {0};
                    int ndi = iv[0], nshr = iv[1], nt = ntens, ns = nstatv, np = nprops, noel = iv[5], npt = iv[6], layer = iv[7], kspt = iv[8];
                    int jstep[4] = {iv[9], 0, 0, 0}, kinc = iv[10];
                    ((UmatFn)stepP)(stress, statev, ddsdde, &sse, &spd, &scd, &rpl, ddsddt, drplde, &drpldt, stran, dstran, tm, &dtime,
                                    &temp, &dtemp, predef, dpred, cmname, &ndi, &nshr, &nt, &ns, props, &np, coords, drot, &pnew,
                                    &celent, f0, f1, &noel, &npt, &layer, &kspt, jstep, &kinc, 80);
                }
                for (int k = 0; k < ntens; ++k)
                    if (!isfinite(stress[k])) { snprintf(why, sizeof why, "the material returned a non-finite stress (element %d, point %d)", iv[5], iv[6]); break; }
            }
            if (why[0]) {
                fail(why);
            } else {
                int zero = 0;
                double o[5] = {sse, spd, scd, rpl, pnew};
                wr(&zero, 4);
                wr(stress, (DWORD)(8 * ntens));
                wr(statev, (DWORD)(8 * nstatv));
                wr(ddsdde, (DWORD)(8 * ntens * ntens));
                wr(o, 40);
                wr(ddsddt, (DWORD)(8 * ntens));
                wr(drplde, (DWORD)(8 * ntens));
                wr(&drpldt, 8);
            }
            free(stress), free(statev), free(stran), free(dstran), free(props), free(ddsdde), free(ddsddt), free(drplde);
        } else {
            fail("unknown request");
            return 1;
        }
    }
}
