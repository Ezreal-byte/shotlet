#include "PlatformMainContext.h"

#import <Foundation/Foundation.h>

namespace shotlet {

// 此文件使用手动引用计数，给纯 C++ 调用方提供作用域内可排空的 pool。
PlatformAutoreleasePool::PlatformAutoreleasePool()
    : m_pool([[NSAutoreleasePool alloc] init]) {}

PlatformAutoreleasePool::~PlatformAutoreleasePool() {
    [(NSAutoreleasePool*)m_pool drain];
}

int RunWithPlatformMainContext(PlatformEntryPoint entryPoint, int argc, char** argv) {
    @autoreleasepool {
        return entryPoint(argc, argv);
    }
}

} // namespace shotlet
