#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "include/nob.h"

#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

typedef enum {
    CLANG = 0,
    GCC = 1,
} Compiler;

typedef struct {
    String_View apple_developer_name;
    String_View ios_device_id;
    String_View android_ndk_location;
    String_View android_sdk_location;
    String_View android_java_home;
} Env;

typedef enum {
    PLATFORM_NATIVE = 0,
    ANDROID,
    IOS,
} Platform;

typedef struct {
    Compiler compiler;
    Platform platform;
    bool optimize;
    bool force_rebuild;
    bool should_run;
    bool device;
} Config;

Config config = {0};
Env env = {0};
Nob_Cmd cmd = {0};

#define BUILD_FOLDER "build"
#define SRC          "src"
#define EXE_NAME     "main.app"
#define CONFIG_FILE_PATH BUILD_FOLDER"/.config"

#define MACOS_TARGET      "11.0"

#define ANDROID_TOOLS "34.0.0"
#define ANDROID_API 34 // 34 seems to be minimal requirement of sdl
#define ANDROID_ABI "arm64-v8a"

// we keep this file between builds, since build/android folder is removed
#define ANDROID_KEYSTORE_FILE BUILD_FOLDER"/keystore_android.keystore"

char *config_platform_name(Platform platform) {
    switch (platform) {
        case PLATFORM_NATIVE: return "Native";
        case ANDROID: return "Android";
        case IOS: return "iOS";
    }
    return NULL;
}

void dump_config_to_file(const char *path, Config config) {
    Nob_String_Builder sb = {0};
    sb_append_cstr(&sb, temp_sprintf("compiler=%d\n", config.compiler));
    sb_append_cstr(&sb, temp_sprintf("optimize=%d\n", config.optimize));
    sb_append_cstr(&sb, temp_sprintf("platform=%d\n", config.platform));
    sb_append_cstr(&sb, temp_sprintf("device=%d\n", config.device));
    write_entire_file(path, sb.items, sb.count);
    sb_free(sb);
}

void log_config_string(Config config) {
    nob_log(NOB_INFO, "Compiler: %s. Optimize=%d. Platform=%s. Device=%d.",
            config.compiler == CLANG ? "clang": "gcc",
            config.optimize,
            config_platform_name(config.platform),
            config.device);
}

#ifdef _WIN32
#define EPOCH_DIFFERENCE 116444736000000000ULL
unsigned long long get_timestamp_usec(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULONGLONG time100ns = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    time100ns -= 116444736000000000ULL;
    return (unsigned long long)(time100ns / 10);
}
#else
unsigned long long get_timestamp_usec(void) {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000000L + time.tv_nsec / 1000;
}
#endif

bool parse_environment(void) {
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file("env", &sb)) return true; // in case we don't care

    String_View sv = sv_from_parts(sb.items, sb.count);
    while (sv.count > 0) {
        String_View line = sv_chop_by_delim(&sv, '\n');

        if (sv_starts_with(line, sv_from_cstr("#"))) continue;
        if (line.count == 0) continue;

        String_View name = sv_chop_by_delim(&line, '=');
        if (name.count == 0) continue;

        if (sv_eq(name, sv_from_cstr("APPLE_DEVELOPER_NAME"))) {
            env.apple_developer_name = line;
        } else if (sv_eq(name, sv_from_cstr("IOS_DEVICE_ID"))) {
            env.ios_device_id = line;
        } else if (sv_eq(name, sv_from_cstr("ANDROID_NDK_LOCATION"))) {
            env.android_ndk_location = line;
        } else if (sv_eq(name, sv_from_cstr("ANDROID_SDK_LOCATION"))) {
            env.android_sdk_location = line;
        } else if (sv_eq(name, sv_from_cstr("ANDROID_JAVA_HOME"))) {
            env.android_java_home = line;
        } else {
            nob_log(NOB_ERROR, "Unexpected variable name '"SV_Fmt"'", SV_Arg(name));
            return false;
        }
    }

    return true;
}

bool parse_config_from_args(int *argc, char ***argv, Config *config) {
    while (*argc > 0) {
        const char *arg = shift_args(argc, argv);
        if (strcmp(arg, "-f") == 0) {
            config->force_rebuild = true;
        } else if (strcmp(arg, "-o") == 0) {
            config->optimize = true;
        } else if (strcmp(arg, "-r") == 0) {
            config->should_run = true;
        } else if (strcmp(arg, "-clang") == 0) {
            config->compiler = CLANG;
        } else if (strcmp(arg, "-gcc") == 0) {
            config->compiler = GCC;
        } else if (strcmp(arg, "-android") == 0) {
            config->platform = ANDROID;
        } else if (strcmp(arg, "-ios") == 0) {
            config->platform = IOS;
        } else if (strcmp(arg, "-device") == 0) {
            config->device = true;
        } else {
            nob_log(NOB_ERROR, "Unexpected argument: %s", arg);
            return false;
        }
    }

#ifndef __APPLE__
    if (config->platform == IOS) {
            nob_log(NOB_ERROR, "macOS is required to build an app for iOS.");
            return false;
    }
#endif

    if (config->platform != PLATFORM_NATIVE) {
        if (config->force_rebuild) {
            nob_log(NOB_ERROR, "Force Rebuild flag makes no sense on iOS or Android, since this script always does a clean build on mobile platforms.");
            return false;
        }

        if (config->compiler == GCC) {
            nob_log(NOB_ERROR, "Compiler flag is not supported on iOS or Android, since we can't freely choose here.");
            return false;
        }
    } else {
        if (config->device) {
            nob_log(NOB_ERROR, "Device flag is only supported with iOS or Android builds for mobile devices. Otherwise it builds for the respective simulator/emulator. For native build, remove the flag.");
            return false;
        }
    }

    return true;
}

bool load_config_from_file(const char *path, Config *config) {
    if (!file_exists(path)) {
        return false;
    }

    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return false;

    Nob_String_View content = sv_from_parts(sb.items, sb.count);
    while (true) {
        Nob_String_View line = sv_chop_by_delim(&content, '\n');
        Nob_String_View key = sv_chop_by_delim(&line, '=');
        Nob_String_View value = line;

        if (nob_sv_eq(key, sv_from_cstr("optimize"))) {
            config->optimize = atoi(value.data);
        } else if (nob_sv_eq(key, sv_from_cstr("compiler"))) {
            config->compiler = atoi(value.data);
        } else if (nob_sv_eq(key, sv_from_cstr("platform"))) {
            config->platform = atoi(value.data);
        } else {
            break;
        }
    }

    sb_free(sb);
    return true;
}

char *uppercase_string(char *s) {
    char *copy = temp_strdup(s);
    char *p = copy;
    while (*p) {
        *p = toupper((unsigned char)*p);
        p++;
    }
    return copy;
}

char *run_cmd_get_stdout(Nob_Cmd *cmd) {
    String_Builder sb = {0};
    Nob_Fd temp_file = fd_open_for_write(BUILD_FOLDER"/temp.txt");
    if (!cmd_run(cmd, .fdout = &temp_file)) return NULL;
    read_entire_file(BUILD_FOLDER"/temp.txt", &sb);
    sb.count -= 1; // maybe bad hack? remove \n at the end
    nob_sb_append_null(&sb);
    return sb.items;
}

void append_cmake_build_flags(Nob_Cmd *cmd) {
#ifndef _WIN32
    long jobs = sysconf(_SC_NPROCESSORS_ONLN);
#else
    long jobs = 4;
#endif
    if (jobs < 1) jobs = 1;
    cmd_append(cmd, temp_sprintf("-j%ld", jobs));
}

bool create_android_keystore(void) {
    if (!file_exists(ANDROID_KEYSTORE_FILE)) {
        cmd_append(&cmd, "keytool");
        cmd_append(&cmd, "-genkeypair");
        cmd_append(&cmd, "-alias", "debug");
        cmd_append(&cmd, "-keyalg", "RSA");
        cmd_append(&cmd, "-keysize", "2048");
        cmd_append(&cmd, "-validity", "10000");
        cmd_append(&cmd, "-keystore", ANDROID_KEYSTORE_FILE);
        cmd_append(&cmd, "-storepass", "android");
        cmd_append(&cmd, "-keypass", "android");
        cmd_append(&cmd, "-dname", "CN=Debug,O=SDLApp,C=US");

        if (!cmd_run(&cmd)) return false;
    }
    return true;
}

// Building frameworks

#define SDL_VERSION "3.2.16"
#define SDL_PATH "lib/SDL-"SDL_VERSION
#define SDL_INCLUDE "-Ilib/SDL-"SDL_VERSION"/include"
#define SDL_FILE BUILD_FOLDER"/libsdl3_native.a"
bool build_sdl(bool force_rebuild) {
    const char *current_dir = get_current_dir_temp();

    if (!file_exists(SDL_FILE) || force_rebuild) {
        set_current_dir(SDL_PATH);

        cmd_append(&cmd, "git", "clean", "-fdx", "build");
        if (!cmd_run(&cmd)) goto error;

        cmd_append(&cmd, "cmake", "-B", BUILD_FOLDER,
             "-DBUILD_SHARED_LIBS=OFF",
             "-DCMAKE_POSITION_INDEPENDENT_CODE=ON");
#ifdef __APPLE__
        cmd_append(&cmd, "-DCMAKE_OSX_DEPLOYMENT_TARGET="MACOS_TARGET);
#endif
        if (!cmd_run(&cmd)) goto error;

        cmd_append(&cmd, "cmake", "--build", BUILD_FOLDER);
        append_cmake_build_flags(&cmd);
        if (!cmd_run(&cmd)) goto error;

#ifdef _WIN32
        const char *output_path = "Debug/SDL3-static.lib";
#else
        const char *output_path = "libSDL3.a";
#endif

        set_current_dir("build"); // sdl's build folder
        if (!copy_file(output_path, "../../../"SDL_FILE)) goto error;
        set_current_dir(current_dir);
    }
    return true;

error:
    set_current_dir(current_dir);
    return false;
}

#define ANDROID_APK_FOLDER ANDROID_BUILD"/apk"
#define SDL_ANDROID_FILE BUILD_FOLDER"/libsdl3_android.so"
bool build_sdl_android(void) {
    const char *current_dir = get_current_dir_temp();

    if (!file_exists(SDL_ANDROID_FILE)) {
        set_current_dir(SDL_PATH);

        cmd_append(&cmd, "git", "clean", "-fdx", "build");
        if (!cmd_run(&cmd)) goto error;

        // TODO: someday try to build it statically for android too
        if (env.android_ndk_location.count == 0) { nob_log(NOB_ERROR, "env ANDROID_NDK_LOCATION not set"); return 1; }
        if (env.android_sdk_location.count == 0) { nob_log(NOB_ERROR, "env ANDROID_SDK_LOCATION not set"); return 1; }

        char *ndk_arg = temp_sprintf("-DCMAKE_TOOLCHAIN_FILE="SV_Fmt"/build/cmake/android.toolchain.cmake", SV_Arg(env.android_ndk_location));
        char *api_arg = temp_sprintf("-DANDROID_PLATFORM=%d", ANDROID_API);
        char *abi_arg = temp_sprintf("-DANDROID_ABI=%s", ANDROID_ABI);

        cmd_append(&cmd, "cmake", "-B", BUILD_FOLDER,
            ndk_arg, api_arg, abi_arg,
            "-DSDL_SHARED=ON",
            "-DSDL_STATIC=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            // "-DCMAKE_BUILD_TYPE=Release"
        );
        if (!cmd_run(&cmd)) goto error;

        cmd_append(&cmd, "cmake", "--build", BUILD_FOLDER);
        append_cmake_build_flags(&cmd);
        if (!cmd_run(&cmd)) goto error;

        set_current_dir("build"); // sdl's build folder
        if (!copy_file("libSDL3.so", "../../../"SDL_ANDROID_FILE)) goto error;
        set_current_dir(current_dir);
    }
    return true;

error:
    set_current_dir(current_dir);
    return false;
}

bool build_sdl_ios(Config *config) {
    const char *current_dir = get_current_dir_temp();

    char *platform = config->device ? "iphoneos" : "iphonesimulator";
    char *sdl_ios_file = temp_sprintf(BUILD_FOLDER"/libsdl3_%s.a", platform);
    char *relative_sdl_ios_file = temp_sprintf("../../../"BUILD_FOLDER"/libsdl3_%s.a", platform);
    char *sysroot_arg = temp_sprintf("-DCMAKE_OSX_SYSROOT=%s", platform);

    if (!file_exists(sdl_ios_file)) {
        set_current_dir(SDL_PATH);

        cmd_append(&cmd, "git", "clean", "-fdx", "build");
        if (!cmd_run(&cmd)) goto error;

        cmd_append(&cmd, "cmake", "-B", BUILD_FOLDER,
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64",
            sysroot_arg,
            "-DSDL_SHARED=OFF",
            "-DSDL_STATIC=ON"
            // "-DCMAKE_BUILD_TYPE=Release",
        );
        if (!cmd_run(&cmd)) goto error;

        cmd_append(&cmd, "cmake", "--build", BUILD_FOLDER);
        if (!cmd_run(&cmd)) goto error;

        set_current_dir("build");
        cmd_append(&cmd, "ls");
        if (!cmd_run(&cmd)) goto error;

        if (!copy_file("libSDL3.a", relative_sdl_ios_file)) goto error;
        set_current_dir(current_dir);
    }

    return true;

error:
    set_current_dir(current_dir);
    return false;
}

// Searching for files

bool allow_h_files(const char *path) {
    size_t len = strlen(path);
    return len > 2 && strcmp(path + len - 2, ".h") == 0;
}

bool allow_java_files(const char *path) {
    size_t len = strlen(path);
    return len > 5 && strcmp(path + len - 5, ".java") == 0;
}

bool allow_c_files(const char *path) {
    size_t len = strlen(path);
    return len > 2 && (strcmp(path + len - 2, ".c") == 0 || strcmp(path + len - 2, ".h") == 0);
}

bool allow_all_files(const char *path) {
    return true;
}

void recursively_collect_files(const char *parent, Nob_File_Paths *out, bool (*filter_function) (const char *)) {
    Nob_File_Paths entries = {0};
    if (!nob_read_entire_dir(parent, &entries)) return;

    for (size_t i = 0; i < entries.count; ++i) {
        const char *sub = entries.items[i];
        if (strcmp(sub, ".") == 0 || strcmp(sub, "..") == 0) continue;
        char *path = temp_sprintf("%s/%s", parent, sub);
        Nob_File_Type t = get_file_type(path);

        if (t == NOB_FILE_DIRECTORY) {
            recursively_collect_files(path, out, filter_function);
        } else if (t == NOB_FILE_REGULAR) {
            if (filter_function(path)) {
                da_append(out, temp_strdup(path));
            }
        }
    }
    da_free(entries);
}

int compare_paths(const void* a, const void* b) {
    const char* sa = *(const char**)a;
    const char* sb = *(const char**)b;
    return strcmp(sa, sb);
}

// Compiler/Linker Flags

void append_frameworks(void) {
#ifdef __APPLE__
    if (config.platform == IOS) {
        cmd_append(&cmd, "-framework", "CoreBluetooth");
        cmd_append(&cmd, "-framework", "CoreMotion");
        cmd_append(&cmd, "-framework", "UIKit");
        cmd_append(&cmd, "-framework", "OpenGLES");
        cmd_append(&cmd, "-framework", "Foundation");
    } else {
        cmd_append(&cmd, "-framework", "AppKit");
        cmd_append(&cmd, "-framework", "AudioUnit");
        cmd_append(&cmd, "-framework", "Carbon");
        cmd_append(&cmd, "-framework", "Cocoa");
        cmd_append(&cmd, "-framework", "ForceFeedback");
        cmd_append(&cmd, "-framework", "GLUT");
        cmd_append(&cmd, "-framework", "OpenGL");
    }

    cmd_append(&cmd, "-framework", "AudioToolbox");
    cmd_append(&cmd, "-framework", "AVFoundation");
    cmd_append(&cmd, "-framework", "CoreAudio");
    cmd_append(&cmd, "-framework", "CoreFoundation");
    cmd_append(&cmd, "-framework", "CoreGraphics");
    cmd_append(&cmd, "-framework", "CoreHaptics");
    cmd_append(&cmd, "-framework", "CoreMedia");
    cmd_append(&cmd, "-framework", "CoreServices");
    cmd_append(&cmd, "-framework", "CoreVideo");
    cmd_append(&cmd, "-framework", "GameController");
    cmd_append(&cmd, "-framework", "IOKit");
    cmd_append(&cmd, "-framework", "Metal");
    cmd_append(&cmd, "-framework", "QuartzCore");
    cmd_append(&cmd, "-framework", "Security");
    cmd_append(&cmd, "-framework", "SystemConfiguration");
    cmd_append(&cmd, "-framework", "UniformTypeIdentifiers");
    cmd_append(&cmd, "-L/opt/homebrew/lib", "-lbz2", "-lz");
#else
    cmd_append(&cmd, "-lm", "-lpthread", "-lz", "-lpng", "-lbz2");
#endif
}

void app_compiler(void) {
#ifdef _WIN32
    cmd_append(&cmd, "cl");
#else
    if (config.compiler == CLANG) {
        cmd_append(&cmd, "clang");
    } else {
        cmd_append(&cmd, "gcc");
    }
#endif
}

void app_default_cmd(void) {
    if (config.platform == PLATFORM_NATIVE) {
        cmd_append(&cmd, "-march=native");
    }

    if (config.optimize) {
        cmd_append(&cmd, "-O3");
    } else {
        cmd_append(&cmd, "-O0", "-g");
        if (config.compiler == CLANG) cmd_append(&cmd, "-fno-limit-debug-info");
    }

    if (config.compiler == CLANG) {
        cmd_append(&cmd, "-fconstant-cfstrings");
    }
#ifdef __APPLE__
    if (config.platform == PLATFORM_NATIVE) cmd_append(&cmd, "-mmacosx-version-min="MACOS_TARGET);
#endif
}

void append_renderer_libraries(void) {
#ifdef __linux__
    Nob_String_Builder sb = {0};
    sb_append_cstr(&sb, "-Wl,--whole-archive,");
    sb_append_cstr(&sb, SDL_FILE);
    sb_append_null(&sb);
    cmd_append(&cmd, sb.items);
    cmd_append(&cmd, "-Wl,--no-whole-archive");
#else
    cmd_append(&cmd, temp_sprintf("-Wl,-force_load,%s", SDL_FILE));
#endif
}

void append_includes(void) {
    cmd_append(&cmd, "-isystem", "include");
    cmd_append(&cmd, SDL_INCLUDE);
}

#define SDL_JAVA_SRC SDL_PATH"/android-project/app/src/main/java/org/libsdl/app"
#define ANDROID_BUILD BUILD_FOLDER"/android"
bool build_app_android(void) {
    if (env.android_ndk_location.count == 0) { nob_log(NOB_ERROR, "env ANDROID_NDK_LOCATION not set"); return 1; }
    if (env.android_sdk_location.count == 0) { nob_log(NOB_ERROR, "env ANDROID_SDK_LOCATION not set"); return 1; }

#ifdef __linux__
    const char *host = "linux-x86_64";
#elif __APPLE__
    const char *host = "darwin-x86_64";
#elif _WIN32
    const char *host = "windows-x86_64";
#else
#  error "Unsupported host for Android build"
#endif

    // build app (as library)

    // TODO: select correct arch
    const char *target_arch = "aarch64-linux";
    char *compiler = temp_sprintf(SV_Fmt"/toolchains/llvm/prebuilt/%s/bin/%s-android%d-clang",
        SV_Arg(env.android_ndk_location), host, target_arch, ANDROID_API);
    char *api_arg = temp_sprintf("-DANDROID_PLATFORM=%d", ANDROID_API);
    char *sdl_arg = temp_sprintf("-L"BUILD_FOLDER);

    cmd_append(&cmd, compiler);
    app_default_cmd();
    cmd_append(&cmd, "-shared");
    cmd_append(&cmd, "-fPIC");
    cmd_append(&cmd, api_arg);
    cmd_append(&cmd, SRC"/main.c");
    append_includes();
    cmd_append(&cmd, sdl_arg, "-lsdl3_android");
    cmd_append(&cmd, "-llog");
    cmd_append(&cmd, "-DRENDERER_SDL3");
    cmd_append(&cmd, "-DOS_ANDROID");
    cmd_append(&cmd, "-g", "-fno-omit-frame-pointer");

    cmd_append(&cmd, "-o");
    cmd_append(&cmd, ANDROID_APK_FOLDER"/lib/"ANDROID_ABI"/libmain.so");
    if (!cmd_run(&cmd)) return false;

    // build java activity
    if (env.android_java_home.count == 0) {
        nob_log(NOB_ERROR, "env JAVA_HOME not set.");
        nob_log(NOB_INFO, "JDK required. When installed, usually located:");
#if __linux__
        nob_log(NOB_INFO, "/usr/lib/jvm/java-{ver}");
#elif __APPLE__
        nob_log(NOB_INFO, "/Library/Java/JavaVirtualMachines/jdk-{ver}.jdk/Contents/Home");
#endif
        return false;
    }

    const char *android_jar = temp_sprintf(SV_Fmt"/platforms/android-%d/android.jar", SV_Arg(env.android_sdk_location), ANDROID_API);
    if (!file_exists(android_jar)) {
        nob_log(NOB_ERROR, "Could not find android.jar. Possibly this android runtime is not installed?");
        nob_log(NOB_INFO, "Looked here: %s", android_jar);

        nob_log(NOB_INFO, "Available platforms:");
        const char *platforms_folder = temp_sprintf(SV_Fmt"/platforms", SV_Arg(env.android_sdk_location));
        cmd_append(&cmd, "ls", platforms_folder);
        if (!cmd_run(&cmd)) return false;

        return false;
    }

    cmd_append(&cmd, "javac");
    cmd_append(&cmd, "-classpath");
    cmd_append(&cmd, android_jar);
    cmd_append(&cmd, "-d", ANDROID_BUILD"/java");
    cmd_append(&cmd, "android/MainActivity.java");

    Nob_File_Paths sdl_java_files = {0};
    recursively_collect_files(SDL_JAVA_SRC, &sdl_java_files, allow_java_files);
    da_foreach(const char*, java_file, &sdl_java_files) {
        cmd_append(&cmd, *java_file);
    }
    if (!cmd_run(&cmd)) return false;

    // build .jar, .dex

    cmd_append(&cmd, "jar", "cf", ANDROID_BUILD"/app.jar", "-C", ANDROID_BUILD"/java", ".");
    if (!cmd_run(&cmd)) return false;

    const char *d8 = temp_sprintf(SV_Fmt"/build-tools/%s/d8", SV_Arg(env.android_sdk_location), ANDROID_TOOLS);
    cmd_append(&cmd, d8,  "--output", ANDROID_BUILD"/apk", ANDROID_BUILD"/app.jar"); // TODO: at some point we will want to add "--release"
    if (!cmd_run(&cmd)) return false;

    // build unsigned apk using manifest
    const char *aapt2 = temp_sprintf(SV_Fmt"/build-tools/%s/aapt2", SV_Arg(env.android_sdk_location), ANDROID_TOOLS);
    cmd_append(&cmd, aapt2);
    cmd_append(&cmd, "link");
    cmd_append(&cmd, "-o", ANDROID_BUILD"/app-unsigned.apk");
    cmd_append(&cmd, "--manifest", "android/AndroidManifest.xml");
    cmd_append(&cmd, "-I", android_jar);
    cmd_append(&cmd, "--version-code", "1");
    cmd_append(&cmd, "--version-name", "1.0");
    if (!cmd_run(&cmd)) return false;

    // add files to apk
    if (!copy_file(SDL_ANDROID_FILE, ANDROID_APK_FOLDER"/lib/"ANDROID_ABI"/libsdl3_android.so")) return false;
    cmd_append(&cmd, "zip", "-g", "-j", ANDROID_BUILD"/app-unsigned.apk", ANDROID_BUILD"/apk/classes.dex");
    if (!cmd_run(&cmd)) return false;

    set_current_dir(ANDROID_APK_FOLDER);
    cmd_append(&cmd, "zip", "-g", "-r", "../app-unsigned.apk", "lib/");
    if (!cmd_run(&cmd)) return false;
    set_current_dir("../../..");

    // align .apk
    const char *zipalign = temp_sprintf(SV_Fmt"/build-tools/%s/zipalign", SV_Arg(env.android_sdk_location), ANDROID_TOOLS);
    cmd_append(&cmd, zipalign, "-v", "4", ANDROID_BUILD"/app-unsigned.apk", ANDROID_BUILD"/app-aligned.apk");
    if (!cmd_run(&cmd)) return false;

    // sign .apk
    const char *apksigner = temp_sprintf(SV_Fmt"/build-tools/%s/apksigner", SV_Arg(env.android_sdk_location), ANDROID_TOOLS);
    cmd_append(&cmd, apksigner);
    cmd_append(&cmd, "sign");
    cmd_append(&cmd, "--ks", ANDROID_KEYSTORE_FILE);
    cmd_append(&cmd, "--ks-pass", "pass:android");
    cmd_append(&cmd, "--key-pass", "pass:android");
    cmd_append(&cmd, "--out", ANDROID_BUILD"/app.apk");
    cmd_append(&cmd, ANDROID_BUILD"/app-aligned.apk");
    if (!cmd_run(&cmd)) return false;

    return true;
}

#define IOS_BUILD BUILD_FOLDER"/ios"
#define IOS_APP_BUNDLE IOS_BUILD"/Player.app"
bool build_app_ios(void) {
    char *platform = config.device ? "iphoneos" : "iphonesimulator";
    char *sdl_ios_file = temp_sprintf(BUILD_FOLDER"/libsdl3_%s.a", platform);

    cmd_append(&cmd, "xcrun", "--sdk", platform, "--show-sdk-path");
    char *ios_sdk_path = run_cmd_get_stdout(&cmd);

    // compile main.c
    char *arch = "arm64";
    cmd_append(&cmd, "clang");
    app_default_cmd();
    cmd_append(&cmd, "-arch", arch);
    cmd_append(&cmd, "-isysroot", ios_sdk_path);
    append_includes();
    cmd_append(&cmd, "-DOS_IOS");
    cmd_append(&cmd, SRC"/main.c");
    cmd_append(&cmd, sdl_ios_file);

    append_frameworks();
    cmd_append(&cmd, "-ObjC");
    cmd_append(&cmd, "-o", IOS_APP_BUNDLE"/Player");
    if (!cmd_run(&cmd)) return false;

    // compile launch screen nib
    const char *nib_input[] = { "ios/LaunchScreen.xib" };
    if (nob_needs_rebuild(BUILD_FOLDER"/LaunchScreen.nib", nib_input, 1)) {
        cmd_append(&cmd, "ibtool", "ios/LaunchScreen.xib", "--compile", BUILD_FOLDER"/LaunchScreen.nib");
        if (!cmd_run(&cmd)) return false;
    }
    if (!copy_file(BUILD_FOLDER"/LaunchScreen.nib", IOS_BUILD"/LaunchScreen.nib")) return false;

    // copy things into app bundle
    if (!copy_file("ios/Info.plist", IOS_APP_BUNDLE"/Info.plist")) return false;

    if (config.device) {
        if (!file_exists("env/profile.mobileprovision")) {
            nob_log(NOB_ERROR, "Appropriate profile file is required at env/profile.mobileprovision. This you can download from your apple developer account.");
            return false;
        }

        if (!file_exists("env/developer_name.txt")) {
            cmd_append(&cmd, "security", "find-identity", "-v", "-p", "codesigning");
            cmd_run(&cmd);

            nob_log(NOB_ERROR, "Please put your account name from the list above into env/developer_name.txt");
            nob_log(NOB_INFO, "Example: Apple Development: Your Name (TEAMID)");
            return false;
        }

        // copy provisioning into app bundle (must be provided by the 
        if (!copy_file("env/profile.mobileprovision", IOS_APP_BUNDLE"/embedded.mobileprovision")) return false;

        // get security data out of the file
        cmd_append(&cmd, "security", "cms", "-D", "-i", "build/ios/Player.app/embedded.mobileprovision");
        Nob_Fd profile_file = fd_open_for_write(BUILD_FOLDER"/ios/profile.plist");
        cmd_run(&cmd, .fdout = &profile_file);

        // create entitlements.plist
        Nob_Fd entitlements_file = fd_open_for_write(BUILD_FOLDER"/ios/entitlements.plist");
        cmd_append(&cmd, "/usr/libexec/PlistBuddy",
            "-x", "-c", "Print :Entitlements",
            (BUILD_FOLDER"/ios/profile.plist"));
        cmd_run(&cmd, .fdout = &entitlements_file);

        // get developer name from file
        String_Builder developer_name_sb = {0};
        read_entire_file("env/developer_name.txt", &developer_name_sb);
        if (developer_name_sb.count == 0) {
            cmd_append(&cmd, "security", "find-identity", "-v", "-p", "codesigning");
            cmd_run(&cmd);

            nob_log(NOB_ERROR, "Please put your account name from the list above into env/developer_name.txt");
            nob_log(NOB_INFO, "Example: Apple Development: Your Name (TEAMID)");
            return false;
        }

        developer_name_sb.count -= 1;
        sb_append_null(&developer_name_sb);
        nob_log(NOB_INFO, "Developer name: '%s'", developer_name_sb.items);

        // sign the binary
        cmd_append(&cmd, "codesign", "--force", "--sign", developer_name_sb.items);
        cmd_append(&cmd, "--preserve-metadata=identifier,entitlements");
        cmd_append(&cmd, "--entitlements", BUILD_FOLDER"/ios/entitlements.plist");
        cmd_append(&cmd, IOS_APP_BUNDLE"/Player");
        cmd_run(&cmd);
    }

    return true;
}

bool build_app_native(void) {
    app_compiler();
    app_default_cmd();
    cmd_append(&cmd, SRC"/main.c");
    append_includes();
    append_renderer_libraries();
    cmd_append(&cmd, "-DRENDERER_SDL3");
    append_frameworks();
    cmd_append(&cmd, "-o", EXE_NAME);
    if (!cmd_run(&cmd)) return false;
    return true;
}

// Main

bool build_clean_all(int argc, char **argv) {
    (void)argc; (void)argv;
    nob_log(NOB_INFO, "Cleaning everything...");

    cmd_append(&cmd, "git", "clean", "-fdx");
    if (!cmd_run(&cmd)) return false;

    nob_cc(&cmd);
    cmd_append(&cmd, "nob.c", "-o", "nob");
    if (!cmd_run(&cmd)) return false;

    return true;
}

bool build_app_config(Config config) {
    unsigned long long start = get_timestamp_usec();

    bool config_did_change = false;
    if (!config.force_rebuild) {
        nob_log(NOB_INFO, "Previously built:");
        log_config_string(config);
        Config saved_config = {0};

        if (!load_config_from_file(CONFIG_FILE_PATH, &saved_config)) {
            config_did_change = true;
        } else if (config.optimize == saved_config.optimize && config.compiler == saved_config.compiler) {
            config_did_change = false;
        } else {
            config_did_change = true;
        }
    }

    minimal_log_level = NOB_NO_LOGS;
    mkdir_if_not_exists(BUILD_FOLDER);
    minimal_log_level = 0;

    Nob_File_Paths app_inputs = {0};
    da_append(&app_inputs, "nob.c");
    recursively_collect_files(SRC, &app_inputs, allow_c_files);

    printf("\n");
    unsigned long long end = get_timestamp_usec();
    nob_log(NOB_INFO, "Took %0.4fs.", (float)(end - start) / 1000000.0f);

    // Run the app
    switch (config.platform) {
        case PLATFORM_NATIVE: {
            bool files_changed = needs_rebuild(EXE_NAME, app_inputs.items, app_inputs.count) == 1;
            if (config_did_change) {
                nob_log(NOB_INFO, "Config changed after last build.");
                log_config_string(config);
            } else if (files_changed) {
                nob_log(NOB_INFO, "Sources were changed.");
            } else if (config.force_rebuild) {
                nob_log(NOB_INFO, "Forced rebuild.");
            } else {
                nob_log(NOB_INFO, "Executable is up to date.");
            }

            if (!build_sdl(false)) return false;

            if (config_did_change || files_changed || config.force_rebuild) {
                if (!build_app_native()) return false;
            }
        } break;
        case ANDROID: {
            cmd_append(&cmd, "rm", "-r", ANDROID_BUILD);
            cmd_run(&cmd);

            mkdir_if_not_exists(ANDROID_BUILD);
            mkdir_if_not_exists(ANDROID_BUILD"/java");
            mkdir_if_not_exists(ANDROID_APK_FOLDER);
            mkdir_if_not_exists(ANDROID_APK_FOLDER"/lib");
            mkdir_if_not_exists(ANDROID_APK_FOLDER"/lib/"ANDROID_ABI);
            mkdir_if_not_exists(ANDROID_APK_FOLDER"/assets");

            if (!create_android_keystore()) return false;
            if (!build_sdl_android()) return false;
            if (!build_app_android()) return false;
        } break;
        case IOS: {
            cmd_append(&cmd, "rm", "-r", IOS_BUILD);
            cmd_run(&cmd);

            if (!mkdir_if_not_exists(BUILD_FOLDER "/ios")) return false;
            if (!mkdir_if_not_exists(BUILD_FOLDER "/ios/bin")) return false;
            if (!mkdir_if_not_exists(IOS_APP_BUNDLE)) return false;

            if (!build_sdl_ios(&config)) return false;
            if (!build_app_ios()) return false;
        } break;
    }

    dump_config_to_file(CONFIG_FILE_PATH, config);

    return true;
}

bool build_app_all_configs(void) {
    if (!build_app_config((Config) {
        .platform = PLATFORM_NATIVE,
        .force_rebuild = true,
    })) return 1;

    if (!build_app_config((Config) {
        .platform = ANDROID,
    })) return 1;

#ifdef __APPLE__
    if (!build_app_config((Config) {
        .platform = IOS,
    })) return 1;

    if (!build_app_config((Config) {
        .platform = IOS,
        .device = true
    })) return 1;
#endif

    return true;
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    if (!parse_environment()) return false;

    shift_args(&argc, &argv);

    if (*(argv) != NULL && strcmp(*(argv), "clean") == 0) {
        if (!build_clean_all(argc, argv) != 0) return 1;
    } else if (*(argv) != NULL && strcmp(*(argv), "sdl") == 0) {
        if (!build_sdl(true) != 0) return 1;
    } else if (*(argv) != NULL && strcmp(*(argv), "test_builds") == 0) {
        if (!build_app_all_configs()) return 1;
    } else {
        if (!parse_config_from_args(&argc, &argv, &config)) return false;
        if (!build_app_config(config)) return 1;
    }

    return 0;
}
