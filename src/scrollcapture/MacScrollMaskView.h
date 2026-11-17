#pragma once
#import <AppKit/AppKit.h>
#include "i18n/Localization.h"

// 滚动采集使用原生透明视图，不依赖 SDL 的窗口背景或 Metal 合成。
@interface ShotletScrollMaskView : NSView
@property(nonatomic) NSRect selectionRect;
@property(nonatomic) BOOL reverseScrollBlocked;
@property(nonatomic) shotlet::Language language;
@end
