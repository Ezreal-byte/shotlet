#include <gtest/gtest.h>
#include "app/ClipboardCopyQueue.h"
#import <ImageIO/ImageIO.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace shotlet;

namespace {
ClipboardPng FakePng(unsigned char value) {
    return ClipboardPng(CFDataCreate(nullptr, &value, 1), [](CFDataRef data) { CFRelease(data); });
}
ClipboardCopyQueue::Request Request(uint64_t sequence) {
    auto pixels = std::shared_ptr<unsigned char>(new unsigned char[4]{static_cast<unsigned char>(sequence), 0, 0, 255},
                                               std::default_delete<unsigned char[]>());
    return {std::move(pixels), 1, 1, sequence, ClipboardCopyQueue::Clock::now()};
}
}

TEST(ClipboardCopyTest, EncodesOnWorkerAndKeepsPngAfterPixelsAreReleased) {
    ClipboardCopyQueue queue;
    auto request = Request(255);
    std::weak_ptr<const unsigned char> pixels = request.pixels;
    queue.Submit(std::move(request));
    auto output = queue.TakeCompleted(true);
    ASSERT_TRUE(output);
    ASSERT_TRUE(output->png);
    EXPECT_TRUE(pixels.expired());
    auto source = CGImageSourceCreateWithData(output->png.get(), nullptr);
    ASSERT_NE(source, nullptr);
    auto image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(CGImageGetWidth(image), 1);
    EXPECT_EQ(CGImageGetHeight(image), 1);
    unsigned char decoded[4]{};
    auto colorSpace = CGColorSpaceCreateDeviceRGB();
    auto context = CGBitmapContextCreate(decoded, 1, 1, 8, 4, colorSpace, kCGImageAlphaPremultipliedLast);
    ASSERT_NE(context, nullptr);
    CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), image);
    EXPECT_EQ(decoded[0], 255);
    EXPECT_EQ(decoded[1], 0);
    EXPECT_EQ(decoded[2], 0);
    EXPECT_EQ(decoded[3], 255);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    CGImageRelease(image);
    CFRelease(source);
}

TEST(ClipboardCopyTest, SlowEncoderDoesNotBlockPollingAndOnlyKeepsLatestPendingRequest) {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, release = false;
    std::vector<unsigned char> encoded;
    const auto mainThread = std::this_thread::get_id();
    std::thread::id workerThread;
    ClipboardCopyQueue queue([&](const unsigned char* pixels, int, int) {
        std::unique_lock lock(mutex);
        workerThread = std::this_thread::get_id();
        if (*pixels == 1) {
            entered = true;
            wake.notify_all();
            wake.wait_for(lock, std::chrono::seconds(2), [&] { return release; });
        }
        encoded.push_back(*pixels);
        return FakePng(*pixels);
    });
    auto first = Request(1);
    std::weak_ptr<const unsigned char> firstPixels = first.pixels;
    queue.Submit(std::move(first));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(wake.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    EXPECT_NE(workerThread, mainThread);
    EXPECT_FALSE(queue.TakeCompleted());
    EXPECT_FALSE(firstPixels.expired());
    auto second = Request(2);
    std::weak_ptr<const unsigned char> secondPixels = second.pixels;
    auto skippedPng = queue.Submit(std::move(second));
    auto latestPng = queue.Submit(Request(3));
    EXPECT_TRUE(secondPixels.expired());
    EXPECT_FALSE(skippedPng.get());
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    wake.notify_all();
    // 立即粘贴可以等待编码结果，后台必须能自行处理排队任务，不能依赖主线程轮询。
    ASSERT_TRUE(latestPng.get());
    auto old = queue.TakeCompleted(true);
    ASSERT_TRUE(old);
    auto latest = old->sequence == 3 ? std::move(old) : queue.TakeCompleted(true);
    ASSERT_TRUE(latest);
    EXPECT_FALSE(latest->superseded);
    EXPECT_EQ(latest->sequence, 3);
    EXPECT_EQ(CFDataGetBytePtr(latest->png.get())[0], 3);
    EXPECT_EQ(encoded, (std::vector<unsigned char>{1, 3}));
    EXPECT_TRUE(firstPixels.expired());
    EXPECT_FALSE(queue.Busy());
}

TEST(ClipboardCopyTest, EncoderFailureDoesNotPreventNextCopy) {
    ClipboardCopyQueue queue([](const unsigned char* pixels, int, int) {
        if (*pixels == 1) throw std::runtime_error("模拟编码失败");
        return FakePng(*pixels);
    });
    queue.Submit(Request(1));
    auto failed = queue.TakeCompleted(true);
    ASSERT_TRUE(failed);
    EXPECT_FALSE(failed->png);
    EXPECT_EQ(failed->error, "模拟编码失败");
    queue.Submit(Request(2));
    auto succeeded = queue.TakeCompleted(true);
    ASSERT_TRUE(succeeded);
    EXPECT_TRUE(succeeded->png);
    EXPECT_FALSE(queue.Busy());
}

TEST(ClipboardCopyTest, RejectsInvalidImageWithoutPublishingClipboard) {
    EXPECT_FALSE(EncodeClipboardPng(nullptr, 10, 10));
    unsigned char pixel[4]{};
    EXPECT_FALSE(EncodeClipboardPng(pixel, 0, 1));
    EXPECT_FALSE(PublishClipboardPng({}));
}

namespace {
NSPasteboard* TestPasteboard(id, SEL) {
    return [NSPasteboard pasteboardWithName:@"org.shotlet.clipboard-unit-tests"];
}
class ClipboardPasteboardTest : public testing::Test {
protected:
    Method method = nullptr;
    IMP original = nullptr;
    void SetUp() override {
        method = class_getClassMethod(NSPasteboard.class, @selector(generalPasteboard));
        original = method_setImplementation(method, reinterpret_cast<IMP>(TestPasteboard));
        if (![NSPasteboard generalPasteboard] || [NSPasteboard generalPasteboard].changeCount < 0)
            GTEST_SKIP() << "当前会话无法连接测试剪贴板";
    }
    void TearDown() override {
        [[NSPasteboard generalPasteboard] releaseGlobally];
        if (original) method_setImplementation(method, original);
    }
};
}

TEST_F(ClipboardPasteboardTest, ImmediatePasteGetsNewPngWithoutMainThreadPolling) {
    ClipboardCopyQueue queue([](const unsigned char* pixels, int width, int height) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return EncodeClipboardPng(pixels, width, height);
    });
    const int version = DeclareClipboardPng(queue.Submit(Request(200)));
    ASSERT_GE(version, 0);
    NSData* data = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG];
    ASSERT_NE(data, nil);
    auto source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    ASSERT_NE(source, nullptr);
    auto image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(CGImageGetWidth(image), 1);
    CGImageRelease(image);
    CFRelease(source);
    const auto completed = queue.TakeCompleted(true);
    ASSERT_TRUE(completed);
    EXPECT_TRUE(completed->png);
}

TEST_F(ClipboardPasteboardTest, LatePublishMaterializesDeclaredPngWithoutBumpingVersion) {
    ClipboardCopyQueue queue;
    const int version = DeclareClipboardPng(queue.Submit(Request(200)));
    ASSERT_GE(version, 0);
    auto completed = queue.TakeCompleted(true);
    ASSERT_TRUE(completed);
    ASSERT_TRUE(completed->png);
    // 生产主路径：编码完成后主线程按版本发布。发布不得扰动 changeCount，
    // 否则后续 IsClipboardPngCurrent 会把本次发布误判为"已被替换"。
    ASSERT_TRUE(IsClipboardPngCurrent(version));
    EXPECT_TRUE(PublishClipboardPng(completed->png, version));
    EXPECT_TRUE(IsClipboardPngCurrent(version));
    NSData* data = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG];
    ASSERT_NE(data, nil);
    auto source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    ASSERT_NE(source, nullptr);
    auto image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    ASSERT_NE(image, nullptr);
    unsigned char decoded[4]{};
    auto colorSpace = CGColorSpaceCreateDeviceRGB();
    auto context = CGBitmapContextCreate(decoded, 1, 1, 8, 4, colorSpace, kCGImageAlphaPremultipliedLast);
    ASSERT_NE(context, nullptr);
    CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), image);
    // 物化后的字节必须能解出本次编码的像素，而不是停留在声明占位。
    EXPECT_EQ(decoded[0], 200);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    CGImageRelease(image);
    CFRelease(source);
}

TEST_F(ClipboardPasteboardTest, NewClipboardContentIsNotOverwrittenByLateEncoding) {
    ClipboardCopyQueue queue;
    const int version = DeclareClipboardPng(queue.Submit(Request(200)));
    ASSERT_GE(version, 0);
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    ASSERT_TRUE([pasteboard setString:@"用户的新内容" forType:NSPasteboardTypeString]);
    auto completed = queue.TakeCompleted(true);
    ASSERT_TRUE(completed);
    EXPECT_FALSE(IsClipboardPngCurrent(version));
    EXPECT_FALSE(PublishClipboardPng(completed->png, version));
    EXPECT_TRUE([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"用户的新内容"]);
}

TEST_F(ClipboardPasteboardTest, FailedEncoderDoesNotHangImmediatePaste) {
    ClipboardCopyQueue queue([](const unsigned char*, int, int) -> ClipboardPng {
        throw std::runtime_error("模拟编码失败");
    });
    ASSERT_GE(DeclareClipboardPng(queue.Submit(Request(1))), 0);
    EXPECT_EQ([[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG], nil);
    auto completed = queue.TakeCompleted(true);
    ASSERT_TRUE(completed);
    EXPECT_FALSE(completed->png);
}

TEST(ClipboardCopyTest, EveryEncodedRequestKeepsItsOwnCompletionAndContext) {
    ClipboardCopyQueue queue([](const unsigned char* p, int, int) { return FakePng(*p); });
    auto first = Request(1);
    auto context = std::make_shared<int>(123); std::weak_ptr<int> weak = context;
    first.context = context; context.reset();
    auto one = queue.Submit(std::move(first)); ASSERT_TRUE(one.get());
    auto two = queue.Submit(Request(2)); ASSERT_TRUE(two.get());
    auto completedOne = queue.TakeCompleted(true); ASSERT_TRUE(completedOne);
    EXPECT_EQ(completedOne->sequence, 1); EXPECT_TRUE(completedOne->superseded);
    EXPECT_EQ(*std::static_pointer_cast<int>(completedOne->context), 123);
    auto completedTwo = queue.TakeCompleted(true); ASSERT_TRUE(completedTwo);
    EXPECT_EQ(completedTwo->sequence, 2); EXPECT_FALSE(completedTwo->superseded);
    EXPECT_FALSE(weak.expired()); completedOne.reset(); EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(queue.Busy());
}

TEST(ClipboardCopyTest, LazyPublicationAcknowledgesOnlySuccessfullyPublishedImage) {
    auto published = std::make_shared<std::atomic<bool>>(false);
    ClipboardCopyQueue queue;
    auto future = queue.Submit(Request(200));
    const int version = DeclareClipboardPng(future, published); ASSERT_GE(version, 0);
    EXPECT_FALSE(published->load());
    @autoreleasepool {
        NSData* data = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG];
        ASSERT_NE(data, nil);
        EXPECT_TRUE(published->load());
    }
    auto result = queue.TakeCompleted(true); ASSERT_TRUE(result); EXPECT_TRUE(result->png);
}
