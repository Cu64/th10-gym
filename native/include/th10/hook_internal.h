#pragma once
#include <cstdint>

void RecordCallback(void* context, std::uint32_t callback_kind,
                    std::int32_t dispatcher_result);
