#include "base.hpp"

void *os_alloc(u64 size) {
    return nullptr;
}

void os_free(void *buf, u64 size) {
}

OSResult cmd_run(Cmd *cmd) {
    return OSResult::OtherError;
}
