// iCube PGO flush shim — see PGOFlush.h.
//
// The __llvm_profile_* runtime is linked into the PVlibDolphin dylib ONLY when it is built with
// -fprofile-generate (DOL_PGO=generate). In a normal shipping build those symbols do not exist
// anywhere, so we MUST NOT create a link-time reference to them — a plain extern (even
// __attribute__((weak_import))) still link-errors with "Undefined symbol ___llvm_profile_*",
// because weak_import only relaxes weak-linking against a dylib that actually exports the symbol,
// not against a symbol that is entirely absent (it comes from a static archive when instrumented).
//
// Solution: resolve the symbols at RUNTIME via dlsym(RTLD_DEFAULT, …). No link-time dependency at
// all — the app links cleanly in every configuration. When the loaded image is instrumented, dlsym
// finds the functions; otherwise it returns NULL and every entry point is a safe no-op. This also
// needs zero coordination between the (separate) xcframework build and the app build.

#include "PGOFlush.h"

#include <dlfcn.h>

// dlsym takes the C symbol name WITHOUT the leading underscore the assembler adds, i.e. the name
// exactly as written in C source. The C function __llvm_profile_write_file becomes the Mach-O
// symbol ___llvm_profile_write_file; dlsym("__llvm_profile_write_file") looks it up correctly.
typedef int (*icube_llvm_write_file_fn)(void);
typedef void (*icube_llvm_set_filename_fn)(const char*);

// PrimeHack for iPad: the runtime's own __llvm_profile_* symbols are private extern in the core
// dylib (they come from a static archive), so dlsym never finds them. The instrumented core exports
// ICubeCorePGOWriteFile / ICubeCorePGOSetFilename (UICommon.cpp, ICUBE_PGO_GENERATE) for this shim;
// the old names stay as a fallback.
static void* ICubePGOLookup(const char* exported_name, const char* runtime_name)
{
  void* sym = dlsym(RTLD_DEFAULT, exported_name);
  return sym ? sym : dlsym(RTLD_DEFAULT, runtime_name);
}

void ICubePGOSetPath(const char* path)
{
  if (!path)
    return;
  icube_llvm_set_filename_fn set_filename = (icube_llvm_set_filename_fn)ICubePGOLookup(
      "ICubeCorePGOSetFilename", "__llvm_profile_set_filename");
  if (set_filename)
    set_filename(path);
}

int ICubePGOFlush(void)
{
  icube_llvm_write_file_fn write_file = (icube_llvm_write_file_fn)ICubePGOLookup(
      "ICubeCorePGOWriteFile", "__llvm_profile_write_file");
  if (!write_file)
    return -1;  // not an instrumented build
  return write_file();
}

int ICubePGOAvailable(void)
{
  return ICubePGOLookup("ICubeCorePGOWriteFile", "__llvm_profile_write_file") != 0;
}
