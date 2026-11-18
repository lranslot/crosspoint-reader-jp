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
  int listen = 0;
  LOCK_TCPIP_CORE();
  active = countList(tcp_active_pcbs);
  timeWait = countList(tcp_tw_pcbs);
  bound = countList(tcp_bound_pcbs);
  // tcp_listen_pcbs is a union of tcp_pcb_listen*; only the link matters here
  // and tcp_pcb_listen's ->next sits at the same offset by design (lwip relies
  // on this itself in TCP_REG/TCP_RMV).
  listen = countList(reinterpret_cast<const struct tcp_pcb*>(tcp_listen_pcbs.pcbs));
  UNLOCK_TCPIP_CORE();

  // tw is the number this was written to find: connections already closed that
  // lwip is still holding for TCP_MSL (60s in this build).
  //
  // The suffix matters as much as the numbers. Burn 16 printed act=0 tw=0
  // bound=0 at both ends of a font family download, and four zeros read as "the
  // instrument is broken" when they may equally mean "nothing was open at that
  // moment" -- which, for both of those sample points, was the truth. A zero
  // has to say which of the two it is (spec 8.8-29, 8.8-40).
  const int written = snprintf(out, outSize, "pcb act=%d tw=%d bound=%d lsn=%d", active, timeWait, bound, listen);
  if (active == 0 && timeWait == 0 && bound == 0 && listen == 0 && written > 0 &&
      static_cast<size_t>(written) < outSize) {
    snprintf(out + written, outSize - static_cast<size_t>(written), " (walked, all lists empty)");
  }
}

}  // namespace net_diag
