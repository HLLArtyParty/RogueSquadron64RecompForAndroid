#if defined(__ANDROID__)
#include <android/log.h>

namespace {
struct AndroidBootLog {
    AndroidBootLog() {
        __android_log_print(ANDROID_LOG_INFO, "RogueSquadron64Recomp", "native library loaded");
    }
} android_boot_log;
}
#endif