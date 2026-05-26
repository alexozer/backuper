#include "base.hpp"
#include "platform.hpp"

#include <process.h>

void *os_alloc(u64 size) {
    // TODO
    return nullptr;
}

void os_free(void *buf, u64 size) {
    // TODO
}

OSResult cmd_run(Cmd *cmd) {
    // TODO
    return OSResult::OtherError;
}

[[noreturn]] void os_exit() {
    _exit(1);
}
