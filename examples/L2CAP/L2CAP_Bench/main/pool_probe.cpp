// Experimental benchmark instrumentation; no SDK sources are modified.
#include "pool_probe.hpp"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <os/os_mempool.h>
#include <os/os_mbuf.h>
#include <cstdint>

#if !CONFIG_BT_LE_MSYS_INIT_IN_CONTROLLER
namespace {
struct Failure {
    int64_t atUs;
    uint32_t blockSize;
    uint16_t blocks, free, minimum;
    char name[OS_MEMPOOL_INFO_NAME_LEN];
};
constexpr size_t capacity = 16;
Failure failures[capacity]{};
size_t used = 0;
uint32_t dropped = 0;
portMUX_TYPE failureMux = portMUX_INITIALIZER_UNLOCKED;
}

extern "C" void* __real_os_memblock_get(os_mempool* pool);
extern "C" void* __wrap_os_memblock_get(os_mempool* pool) {
    void* block = __real_os_memblock_get(pool);
    // Successful allocations have no logging, locking or timestamp calls.
    if (block || !pool) return block;
    const auto atUs = esp_timer_get_time();
    portENTER_CRITICAL(&failureMux);
    if (used < capacity) {
        auto& f = failures[used++];
        f.atUs = atUs;
        f.blockSize = pool->mp_block_size;
        f.blocks = pool->mp_num_blocks;
        f.free = pool->mp_num_free;
        f.minimum = pool->mp_min_free;
        size_t i = 0;
        if (pool->name) {
            for (; i + 1 < sizeof(f.name) && pool->name[i]; ++i) f.name[i] = pool->name[i];
        }
        f.name[i] = '\0';
    } else ++dropped;
    portEXIT_CRITICAL(&failureMux);
    return nullptr;
}

void benchEmitPoolFailures(void (*emit)(const char*, cJSON*)) {
    Failure snapshot[capacity];
    size_t count;
    uint32_t lost;
    portENTER_CRITICAL(&failureMux);
    count = used;
    for (size_t i = 0; i < count; ++i) snapshot[i] = failures[i];
    lost = dropped;
    used = 0;
    dropped = 0;
    portEXIT_CRITICAL(&failureMux);
    for (size_t i = 0; i < count; ++i) {
        const auto& f = snapshot[i];
        auto object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, "pool", f.name);
        cJSON_AddNumberToObject(object, "at_us", f.atUs);
        cJSON_AddNumberToObject(object, "block_size", f.blockSize);
        cJSON_AddNumberToObject(object, "blocks", f.blocks);
        cJSON_AddNumberToObject(object, "free_after_failure", f.free);
        cJSON_AddNumberToObject(object, "minimum_free", f.minimum);
        emit("pool-failure", object);
    }
    if (count || lost) {
        auto object = cJSON_CreateObject();
        cJSON_AddNumberToObject(object, "captured", count);
        cJSON_AddNumberToObject(object, "dropped", lost);
        emit("pool-failure-summary", object);
        benchEmitPoolState(emit);
    }
}
#else
// S31 routes native allocations through controller-owned r_os_* functions.
// The host allocator hook and pool registry do not cover that backend.
void benchEmitPoolFailures(void (*)(const char*, cJSON*)) {}
#endif

void benchEmitPoolState(void (*emit)(const char*, cJSON*)) {
#if CONFIG_BT_LE_MSYS_INIT_IN_CONTROLLER
    auto object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "source", "controller-msys");
    // Preserve API returns verbatim. On this controller backend the observed
    // num_free return can exceed count; these are not a fixed-capacity proof.
    cJSON_AddNumberToObject(object, "os_msys_count_return", os_msys_count());
    cJSON_AddNumberToObject(object, "os_msys_num_free_return", os_msys_num_free());
    cJSON_AddBoolToObject(object, "fixed_capacity_interpretation_supported", false);
    cJSON_AddBoolToObject(object, "per_pool_registry_available", false);
    emit("controller-msys-counters", object);
#else
    os_mempool_info info{};
    os_mempool* pool = nullptr;
    while ((pool = os_mempool_info_get_next(pool, &info))) {
        auto object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, "pool", info.omi_name);
        cJSON_AddNumberToObject(object, "block_size", info.omi_block_size);
        cJSON_AddNumberToObject(object, "blocks", info.omi_num_blocks);
        cJSON_AddNumberToObject(object, "free", info.omi_num_free);
        cJSON_AddNumberToObject(object, "minimum_free", info.omi_min_free);
        emit("pool-state-after-command", object);
    }
#endif
}
