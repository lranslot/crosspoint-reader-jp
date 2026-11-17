#pragma once

#include <cstddef>

// How many TCP connections lwip is still holding, and in which state.
//
// WHY THIS EXISTS: three builds of measurement have ruled out the obvious
// suspects for the heap damage a burst of HTTPS downloads leaves behind. The
// wolfSSL receive buffer is not it -- burn 14 measured inbuf=2065 while the
// largest free block fell 23,552 bytes in the same session. The glyph cache is
// a contributor but a small one -- throttling the progress repaint to 5% moved
// the loss only from 24,576 to 23,552. Free heap returns to within 8 bytes of
// where it started every time; only contiguous space is lost. So the remaining
// suspect is the connection itself, and specifically what survives close():
//
//   CONFIG_LWIP_TCP_SND_BUF_DEFAULT = 5744
//   CONFIG_LWIP_TCP_WND_DEFAULT     = 5760     (~11.5 KB per socket)
//   CONFIG_LWIP_TCP_MSL             = 60000    (TIME-WAIT lasts a minute)
//
// A closed TCP connection does not disappear: lwip moves its PCB to
// tcp_tw_pcbs and keeps it for TCP_MSL. Every download in burn 14 happened
// inside one such minute, so each new connection was made while the previous
// ones were still held. That is a hypothesis, and this is how it gets tested
// rather than argued.
//
// WHAT CANNOT BE MEASURED: lwip's own memory accounting is compiled out --
// sdkconfig has "CONFIG_LWIP_STATS is not set", so lwip_stats and the MEMP_*
// counters do not exist in this build and cannot report bytes held. Turning
// them on means rebuilding lwip, which is a separate decision. The PCB lists,
// by contrast, need no stats: lwip declares them in lwip/priv/tcp_priv.h, which
// ships in the SDK include tree. Counting entries is therefore possible and
// counting bytes is not, so this reports connections, not kilobytes.
//
// The lists are owned by the TCP/IP task. CONFIG_LWIP_TCPIP_CORE_LOCKING=y in
// this build, so they are walked under LOCK_TCPIP_CORE().
namespace net_diag {

// Writes "pcb act=N tw=M bound=K" into out, for a heapdiag detail field.
// Always NUL-terminates. Writes "pcb unavailable" if the lists cannot be read.
void describeTcpPcbs(char* out, size_t outSize);

}  // namespace net_diag
