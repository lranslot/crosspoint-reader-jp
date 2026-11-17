#include "NetDiag.h"

#include <cstdio>

// A private lwip header, used deliberately: the public API has no way to ask
// how many connections are still held, and the PCB lists are the only place
// that answer exists in a build without CONFIG_LWIP_STATS. If an SDK update
// moves or renames them this stops compiling, which is the right failure for a
// diagnostic -- one that silently reads the wrong memory is worse than none
// (spec 8.8-30).
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>

namespace net_diag {

namespace {
// tcp_pcb lists are singly linked through ->next and are only consistent while
// the core lock is held. The cap is there so a corrupted list cannot spin
// forever inside a diagnostic; NUM_TCP_PCB_LISTS worth of sockets is far below
// it (CONFIG_LWIP_MAX_SOCKETS is 16).
constexpr int MAX_WALK = 64;

int countList(const struct tcp_pcb* head) {
  int n = 0;
  for (const struct tcp_pcb* pcb = head; pcb != nullptr && n < MAX_WALK; pcb = pcb->next) {
    n++;
  }
  return n;
}
}  // namespace

void describeTcpPcbs(char* out, const size_t outSize) {
  if (!out || outSize == 0) return;

  int active = 0;
  int timeWait = 0;
  int bound = 0;
  LOCK_TCPIP_CORE();
  active = countList(tcp_active_pcbs);
  timeWait = countList(tcp_tw_pcbs);
  bound = countList(tcp_bound_pcbs);
  UNLOCK_TCPIP_CORE();

  // tw is the number this was written to find: connections already closed that
  // lwip is still holding for TCP_MSL (60s in this build).
  snprintf(out, outSize, "pcb act=%d tw=%d bound=%d", active, timeWait, bound);
}

}  // namespace net_diag
