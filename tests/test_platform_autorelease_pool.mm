#include "platform/PlatformMainContext.h"
#include <gtest/gtest.h>
#include <stdexcept>
#import <Foundation/Foundation.h>

// 使用真实 Objective-C 对象观测释放时机，不用 SDL 或窗口测试桩。
@interface PoolLifetimeProbe : NSObject {
    int* m_destroyed;
}
- (id)initWithCounter:(int*)counter;
@end

@implementation PoolLifetimeProbe
- (id)initWithCounter:(int*)counter {
    self = [super init];
    if (self) m_destroyed = counter;
    return self;
}
- (void)dealloc {
    ++*m_destroyed;
    [super dealloc];
}
@end

TEST(PlatformAutoreleasePoolTest, DrainsEachWorkUnitBeforeProcessExit) {
    int destroyed = 0;
    shotlet::PlatformAutoreleasePool outer;
    for (int frame = 0; frame < 10; ++frame) {
        {
            shotlet::PlatformAutoreleasePool inner;
            [[[PoolLifetimeProbe alloc] initWithCounter:&destroyed] autorelease];
            EXPECT_EQ(destroyed, frame);
        }
        EXPECT_EQ(destroyed, frame + 1);
    }
}

TEST(PlatformAutoreleasePoolTest, ExplicitOwnershipSurvivesPoolDrain) {
    int destroyed = 0;
    PoolLifetimeProbe* retained;
    {
        shotlet::PlatformAutoreleasePool pool;
        retained = [[[PoolLifetimeProbe alloc] initWithCounter:&destroyed] autorelease];
        [retained retain];
    }
    EXPECT_EQ(destroyed, 0);
    [retained release];
    EXPECT_EQ(destroyed, 1);
}

TEST(PlatformAutoreleasePoolTest, ExceptionUnwindingDrainsPool) {
    int destroyed = 0;
    EXPECT_THROW({
        shotlet::PlatformAutoreleasePool pool;
        [[[PoolLifetimeProbe alloc] initWithCounter:&destroyed] autorelease];
        throw std::runtime_error("模拟工作单元失败");
    }, std::runtime_error);
    EXPECT_EQ(destroyed, 1);
}

TEST(PlatformAutoreleasePoolTest, NestedPoolDoesNotReleaseOuterObjects) {
    int outerDestroyed = 0;
    int innerDestroyed = 0;
    {
        shotlet::PlatformAutoreleasePool outer;
        [[[PoolLifetimeProbe alloc] initWithCounter:&outerDestroyed] autorelease];
        {
            shotlet::PlatformAutoreleasePool inner;
            [[[PoolLifetimeProbe alloc] initWithCounter:&innerDestroyed] autorelease];
        }
        EXPECT_EQ(innerDestroyed, 1);
        EXPECT_EQ(outerDestroyed, 0);
    }
    EXPECT_EQ(outerDestroyed, 1);
}
