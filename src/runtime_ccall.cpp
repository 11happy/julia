// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include <map>
#include <string>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>

#include "julia.h"
#include "julia_internal.h"
#include "processor.h"
#include "julia_assert.h"

#ifndef _OS_WINDOWS_
#include <sys/mman.h>
#if defined(_OS_DARWIN_) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

using namespace llvm;

// --- library symbol lookup ---

// map from user-specified lib names to handles
static std::map<std::string, void*> libMap;
static jl_mutex_t libmap_lock;
extern "C"
void *jl_get_library_(const char *f_lib, int throw_err)
{
    if (f_lib == NULL)
        return jl_RTLD_DEFAULT_handle;
    if (f_lib == JL_EXE_LIBNAME)
        return jl_exe_handle;
    if (f_lib == JL_LIBJULIA_INTERNAL_DL_LIBNAME)
        return jl_libjulia_internal_handle;
    if (f_lib == JL_LIBJULIA_DL_LIBNAME)
        return jl_libjulia_handle;
    JL_LOCK(&libmap_lock);
    // This is the only operation we do on the map, which doesn't invalidate
    // any references or iterators.
    void **map_slot = &libMap[f_lib];
    void *hnd = *map_slot;
    if (hnd == NULL) {
        hnd = jl_load_dynamic_library(f_lib, JL_RTLD_DEFAULT, throw_err);
        if (hnd != NULL)
            *map_slot = hnd;
    }
    JL_UNLOCK(&libmap_lock);
    return hnd;
}

extern "C" JL_DLLEXPORT
void *jl_load_and_lookup(const char *f_lib, const char *f_name, _Atomic(void*) *hnd)
{
    void *handle = jl_atomic_load_acquire(hnd);
    if (!handle)
        jl_atomic_store_release(hnd, (handle = jl_get_library(f_lib)));
    void * ptr;
    jl_dlsym(handle, f_name, &ptr, 1);
    return ptr;
}

// jl_load_and_lookup, but with library computed at run time on first call
extern "C" JL_DLLEXPORT
void *jl_lazy_load_and_lookup(jl_value_t *lib_val, const char *f_name)
{
    void *lib_ptr;

    if (jl_is_symbol(lib_val))
        lib_ptr = jl_get_library(jl_symbol_name((jl_sym_t*)lib_val));
    else if (jl_is_string(lib_val))
        lib_ptr = jl_get_library(jl_string_data(lib_val));
    else if (jl_libdl_dlopen_func != NULL) {
        // Call `dlopen(lib_val)`; this is the correct path for the `LazyLibrary` case,
        // but it also takes any other value, and so we define `dlopen(x::Any) = throw(TypeError(...))`.
        lib_ptr = jl_unbox_voidpointer(jl_apply_generic(jl_libdl_dlopen_func, &lib_val, 1));
    } else
        jl_type_error("ccall", (jl_value_t*)jl_symbol_type, lib_val);
    void *ptr;
    jl_dlsym(lib_ptr, f_name, &ptr, 1);
    return ptr;
}

// miscellany
std::string jl_get_cpu_name_llvm(void)
{
    return llvm::sys::getHostCPUName().str();
}

std::string jl_get_cpu_features_llvm(void)
{
    StringMap<bool> HostFeatures;
    llvm::sys::getHostCPUFeatures(HostFeatures);
    std::string attr;
    for (auto &ele: HostFeatures) {
        if (ele.getValue()) {
            if (!attr.empty()) {
                attr.append(",+");
            }
            else {
                attr.append("+");
            }
            attr.append(ele.getKey().str());
        }
    }
    // Explicitly disabled features need to be added at the end so that
    // they are not re-enabled by other features that implies them by default.
    for (auto &ele: HostFeatures) {
        if (!ele.getValue()) {
            if (!attr.empty()) {
                attr.append(",-");
            }
            else {
                attr.append("-");
            }
            attr.append(ele.getKey().str());
        }
    }
    return attr;
}

extern "C" JL_DLLEXPORT
jl_value_t *jl_get_JIT(void)
{
    const std::string& HostJITName = "ORCJIT";
    return jl_pchar_to_string(HostJITName.data(), HostJITName.size());
}

#ifndef MAXHOSTNAMELEN
# define MAXHOSTNAMELEN 256
#endif

// Form a file name from a pattern made by replacing tokens,
// similar to many of those provided by ssh_config TOKENS:
//
//           %%    A literal `%'.
//           %p    The process PID
//           %d    Local user's home directory.
//           %i    The local user ID.
//           %L    The local hostname.
//           %l    The local hostname, including the domain name.
//           %u    The local username.
std::string jl_format_filename(StringRef output_pattern)
{
    std::string buf;
    raw_string_ostream outfile(buf);
    bool special = false;
    char hostname[MAXHOSTNAMELEN + 1];
    uv_passwd_t pwd;
    bool got_pwd = false;
    for (auto c : output_pattern) {
        if (special) {
            if (!got_pwd && (c == 'i' || c == 'd' || c == 'u')) {
                int r = uv_os_get_passwd(&pwd);
                if (r == 0)
                    got_pwd = true;
            }
            switch (c) {
            case 'p':
                outfile << uv_os_getpid();
                break;
            case 'd':
                if (got_pwd)
                    outfile << pwd.homedir;
                break;
            case 'i':
                if (got_pwd)
                    outfile << pwd.uid;
                break;
            case 'l':
            case 'L':
                if (gethostname(hostname, sizeof(hostname)) == 0) {
                    hostname[sizeof(hostname) - 1] = '\0'; /* Null terminate, just to be safe. */
                    outfile << hostname;
                }
#ifndef _OS_WINDOWS_
                if (c == 'l' && getdomainname(hostname, sizeof(hostname)) == 0) {
                    hostname[sizeof(hostname) - 1] = '\0'; /* Null terminate, just to be safe. */
                    outfile << hostname;
                }
#endif
                break;
            case 'u':
                if (got_pwd)
                    outfile << pwd.username;
                break;
            default:
                outfile << c;
                break;
            }
            special = false;
        }
        else if (c == '%') {
            special = true;
        }
        else {
            outfile << c;
        }
    }
    if (got_pwd)
        uv_os_free_passwd(&pwd);
    return outfile.str();
}

extern "C" JL_DLLEXPORT char *jl_format_filename(const char *output_pattern)
{
    return strdup(jl_format_filename(StringRef(output_pattern)).c_str());
}


static uv_mutex_t trampoline_lock; // for accesses to the cache and freelist

static void *trampoline_freelist;

static void *trampoline_alloc() JL_NOTSAFEPOINT // lock taken by caller
{
    const int sz = 64; // oversized for most platforms. todo: use precise value?
    if (!trampoline_freelist) {
        int last_errno = errno;
#ifdef _OS_WINDOWS_
        DWORD last_error = GetLastError();
        void *mem = VirtualAlloc(NULL, jl_page_size,
                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (mem == NULL)
            jl_throw(jl_memory_exception);
        SetLastError(last_error);
#else
        void *mem = mmap(0, jl_page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        errno = last_errno;
        if (mem == MAP_FAILED)
            jl_throw(jl_memory_exception);
#endif
        errno = last_errno;
        void *next = NULL;
        assert(sz < jl_page_size);
        for (size_t i = 0; i + sz <= jl_page_size; i += sz) {
            void **curr = (void**)((char*)mem + i);
            *curr = next;
            next = (void*)curr;
        }
        trampoline_freelist = next;
    }
    void *tramp = trampoline_freelist;
    trampoline_freelist = *(void**)tramp;
    return tramp;
}

static void trampoline_free(void *tramp) JL_NOTSAFEPOINT    // lock taken by caller
{
    *(void**)tramp = trampoline_freelist;
    trampoline_freelist = tramp;
}

static void trampoline_deleter(void **f) JL_NOTSAFEPOINT
{
    void *tramp = f[0];
    void *fobj = f[1];
    void *cache = f[2];
    void *nval = f[3];
    f[0] = NULL;
    f[2] = NULL;
    f[3] = NULL;
    uv_mutex_lock(&trampoline_lock);
    if (tramp)
        trampoline_free(tramp);
    if (fobj && cache)
        ptrhash_remove((htable_t*)cache, fobj);
    if (nval)
        free(nval);
    uv_mutex_unlock(&trampoline_lock);
}

typedef void *(*init_trampoline_t)(void *tramp, void **nval) JL_NOTSAFEPOINT;

// Use of `cache` is not clobbered in JL_TRY
JL_GCC_IGNORE_START("-Wclobbered")
extern "C" JL_DLLEXPORT
jl_value_t *jl_get_cfunction_trampoline(
    // dynamic inputs:
    jl_value_t *fobj,
    jl_datatype_t *result_type,
    // call-site constants:
    htable_t *cache, // weakref htable indexed by (fobj, vals)
    jl_svec_t *fill,
    init_trampoline_t init_trampoline,
    jl_unionall_t *env,
    jl_value_t **vals)
{
    // lookup (fobj, vals) in cache
    uv_mutex_lock(&trampoline_lock);
    if (!cache->table)
        htable_new(cache, 1);
    if (fill != jl_emptysvec) {
        htable_t **cache2 = (htable_t**)ptrhash_bp(cache, (void*)vals);
        cache = *cache2;
        if (cache == HT_NOTFOUND) {
            cache = htable_new((htable_t*)malloc_s(sizeof(htable_t)), 1);
            *cache2 = cache;
        }
    }
    void *tramp = ptrhash_get(cache, (void*)fobj);
    uv_mutex_unlock(&trampoline_lock);
    if (tramp != HT_NOTFOUND) {
        assert((jl_datatype_t*)jl_typeof(tramp) == result_type);
        return (jl_value_t*)tramp;
    }

    // not found, allocate a new one
    size_t n = jl_svec_len(fill);
    void **nval = (void**)malloc_s(sizeof(void*) * (n + 1));
    nval[0] = (void*)fobj;
    jl_value_t *result;
    JL_TRY {
        for (size_t i = 0; i < n; i++) {
            jl_value_t *sparam_val = jl_instantiate_type_in_env(jl_svecref(fill, i), env, vals);
            if (sparam_val != (jl_value_t*)jl_any_type)
                if (!jl_is_concrete_type(sparam_val) || !jl_is_immutable(sparam_val))
                    sparam_val = NULL;
            nval[i + 1] = (void*)sparam_val;
        }
        int permanent =
            (result_type == jl_voidpointer_type) ||
            jl_is_concrete_type(fobj) ||
            (((jl_datatype_t*)jl_typeof(fobj))->instance == fobj);
        if (jl_is_unionall(fobj)) {
            jl_value_t *uw = jl_unwrap_unionall(fobj);
            if (jl_is_datatype(uw) && ((jl_datatype_t*)uw)->name->wrapper == fobj)
                permanent = true;
        }
        if (permanent) {
            result = jl_gc_permobj(sizeof(jl_taggedvalue_t) + jl_datatype_size(result_type), result_type);
            memset(result, 0, jl_datatype_size(result_type));
        }
        else {
            result = jl_new_struct_uninit(result_type);
        }
        if (result_type != jl_voidpointer_type) {
            assert(jl_datatype_size(result_type) == sizeof(void*) * 4);
            ((void**)result)[1] = (void*)fobj;
        }
        if (!permanent) {
            jl_task_t *ct = jl_current_task;
            jl_gc_add_ptr_finalizer(ct->ptls, result, (void*)(uintptr_t)&trampoline_deleter);
            ((void**)result)[2] = (void*)cache;
            ((void**)result)[3] = (void*)nval;
        }
    }
    JL_CATCH {
        free(nval);
        jl_rethrow();
    }
    uv_mutex_lock(&trampoline_lock);
    tramp = trampoline_alloc();
    ((void**)result)[0] = tramp;
    init_trampoline(tramp, nval);
    ptrhash_put(cache, (void*)fobj, result);
    uv_mutex_unlock(&trampoline_lock);
    return result;
}
JL_GCC_IGNORE_STOP

void jl_init_runtime_ccall(void)
{
    JL_MUTEX_INIT(&libmap_lock, "libmap_lock");
    uv_mutex_init(&trampoline_lock);
}
