#ifndef BLUE_RENDER_METAL_METAL_TYPES_H_
#define BLUE_RENDER_METAL_METAL_TYPES_H_

// The Metal and Core Animation declarations every internal render header
// needs. Internal render headers name Objective-C types (`id<MTLDevice>`, ...)
// and are compiled as Objective-C++ with ARC; the public render headers stay
// pure C++ and never include this.

#ifndef __OBJC__
#error "render internals are Objective-C++; include them from .mm files only"
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#endif  // BLUE_RENDER_METAL_METAL_TYPES_H_
