#define MOZ_UNIFIED_BUILD
#include "threading/Thread.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "threading/Thread.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "threading/Thread.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "threading/windows/CpuCount.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "threading/windows/CpuCount.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "threading/windows/CpuCount.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "threading/windows/WindowsThread.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "threading/windows/WindowsThread.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "threading/windows/WindowsThread.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Activation.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Activation.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Activation.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArgumentsObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArgumentsObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArgumentsObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArrayBufferObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArrayBufferObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArrayBufferObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif