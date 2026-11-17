#pragma once

#include <SDL3/SDL.h>
#include <array>
#include "Logger.h"

namespace shotlet::EventUtils {

// 截图窗口销毁后清掉残留事件，防止嵌入宿主的下一次截图收到旧窗口事件。
    inline void FlushPendingEvents(const char *reason) {
        constexpr int EventBatchSize = 64;
        Logger::Debug("[SDL] flush pending events begin reason={}", reason);
        SDL_PumpEvents();

        int totalFlushed = 0;
        std::array<SDL_Event, EventBatchSize> events;
        for (;;) {
            const int count =
                    SDL_PeepEvents(events.data(), EventBatchSize, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
            if (count <= 0)
                break;
            totalFlushed += count;
        }

        Logger::Debug("[SDL] flushed pending events reason={} count={}", reason, totalFlushed);
    }

} // namespace shotlet::EventUtils
