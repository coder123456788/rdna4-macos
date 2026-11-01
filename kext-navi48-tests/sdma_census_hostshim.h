// sdma_census_hostshim.h — the one kernel declaration sdma_v7_0.cpp needs that
// tests/dalshim does not carry. Passed with -include when the suite compiles the
// product's SDMA translation unit on the host; the kext never sees this file.
//
// IODelay() is declared by MacKernelSDK's IOKit headers (real time for the SDMA
// MCU handshakes). Nothing on the census path reaches it — the suite fails if a
// host stub is called at all — but the file it lives in cannot compile without
// the name existing.
#pragma once
static inline void IODelay(unsigned int microseconds) { (void)microseconds; }
