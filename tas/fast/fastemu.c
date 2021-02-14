/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include <tas_memif.h>
#include <utils_log.h>

#include "internal.h"
#include "fastemu.h"


static unsigned poll_rx(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_queues(struct dataplane_context *ctx, uint32_t ts)  __attribute__((noinline));
static unsigned poll_kernel(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman_fwd(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static void poll_scale(struct dataplane_context *ctx);

static inline uint8_t bufcache_prealloc(struct dataplane_context *ctx, uint16_t num,
    struct network_buf_handle ***handles);
static inline void bufcache_alloc(struct dataplane_context *ctx, uint16_t num);
static inline void bufcache_free(struct dataplane_context *ctx,
    struct network_buf_handle *handle);

static inline void tx_flush(struct dataplane_context *ctx);
static inline void tx_send(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, uint16_t off, uint16_t len);

static void arx_cache_flush(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));

int dataplane_init(void)
{
  if (FLEXNIC_INTERNAL_MEM_SIZE < sizeof(struct flextcp_pl_mem)) {
    fprintf(stderr, "dataplane_init: internal flexnic memory size not "
        "sufficient (got %llx, need %zx)\n", FLEXNIC_INTERNAL_MEM_SIZE,
        sizeof(struct flextcp_pl_mem));
    return -1;
  }

  if (fp_cores_max > FLEXNIC_PL_APPST_CTX_MCS) {
    fprintf(stderr, "dataplane_init: more cores than FLEXNIC_PL_APPST_CTX_MCS "
        "(%u)\n", FLEXNIC_PL_APPST_CTX_MCS);
    return -1;
  }
  if (FLEXNIC_PL_FLOWST_NUM > FLEXNIC_NUM_QMQUEUES) {
    fprintf(stderr, "dataplane_init: more flow states than queue manager queues"
        "(%u > %llu)\n", FLEXNIC_PL_FLOWST_NUM, FLEXNIC_NUM_QMQUEUES);
    return -1;
  }

  return 0;
}

int dataplane_context_init(struct dataplane_context *ctx)
{
  char name[32];

  /* initialize forwarding queue */
  sprintf(name, "qman_fwd_ring_%u", ctx->id);
  if ((ctx->qman_fwd_ring = rte_ring_create(name, 32 * 1024, rte_socket_id(),
          RING_F_SC_DEQ)) == NULL)
  {
    fprintf(stderr, "initializing rte_ring_create");
    return -1;
  }

  /* initialize queue manager */
  if (qman_thread_init(ctx) != 0) {
    fprintf(stderr, "initializing qman thread failed\n");
    return -1;
  }

  /* initialize network queue */
  if (network_thread_init(ctx) != 0) {
    fprintf(stderr, "initializing rx thread failed\n");
    return -1;
  }

  ctx->poll_next_ctx = ctx->id;

  ctx->evfd = eventfd(0, 0);
  assert(ctx->evfd != -1);
  ctx->ev.epdata.event = EPOLLIN;
  int r = rte_epoll_ctl(RTE_EPOLL_PER_THREAD, EPOLL_CTL_ADD, ctx->evfd, &ctx->ev);
  assert(r == 0);
  fp_state->kctx[ctx->id].evfd = ctx->evfd;

  return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx)
{
}

void dataplane_loop(struct dataplane_context *ctx)
{
  uint32_t ts, startwait = 0;
  uint64_t cyc, prev_cyc;
  int was_idle = 1;

  TAS_LOG(INFO, MAIN, "lcore %d: Entering dataplane_loop()\n", rte_lcore_id());

  while (!exited) {
    unsigned n = 0;
    unsigned ret = 0;

    /* count cycles of previous iteration if it was busy */
    prev_cyc = cyc;
    cyc = rte_get_tsc_cycles();
    if (!was_idle)
      ctx->loadmon_cyc_busy += cyc - prev_cyc;


    ts = qman_timestamp(cyc);

    STATS_TS(start);
    n += poll_rx(ctx, ts);
    STATS_TS(rx);
    STATS_ATOMIC_ADD(ctx, cyc_rx, rx - start);

    tx_flush(ctx);
    STATS_TS(acktx);
    STATS_ATOMIC_ADD(ctx, cyc_tx, acktx - rx);

    n += poll_qman_fwd(ctx, ts);

    STATS_TS(poll_qman_start);
    ret = poll_qman(ctx, ts);
    n+=ret;
    STATS_TS(poll_qman_end);
    
    if(ret>0)
    {
        STATS_ATOMIC_ADD(ctx, cyc_qm_useful, poll_qman_end - poll_qman_start);
    }

    STATS_ATOMIC_ADD(ctx, cyc_qm, poll_qman_end - poll_qman_start);

    //n += poll_queues(ctx, ts);
    ret = poll_queues(ctx, ts);
    n+=ret;
    STATS_TS(qs_end);
    STATS_ATOMIC_ADD(ctx, cyc_qs, qs_end - poll_qman_end);

    if(ret>0)
    {
        STATS_ATOMIC_ADD(ctx, cyc_qs_useful,  qs_end - poll_qman_end);
    }

    n += poll_kernel(ctx, ts);
    STATS_TS(sp);
    STATS_ATOMIC_ADD(ctx, cyc_sp, sp - qs_end);

    /* flush transmit buffer */
    tx_flush(ctx);
    STATS_TS(tx);
    STATS_ATOMIC_ADD(ctx, cyc_tx, tx - sp);

    if (ctx->id == 0)
      poll_scale(ctx);

    if(UNLIKELY(n == 0)) {
      was_idle = 1;

      if(startwait == 0) {
        startwait = ts;
      } else if (config.fp_interrupts && ts - startwait >= POLL_CYCLE) {
        // Idle -- wait for interrupt or data from apps/kernel
        int r = network_rx_interrupt_ctl(&ctx->net, 1);

        // Only if device running
        if(r == 0) {
          uint32_t timeout_us = qman_next_ts(&ctx->qman, ts);
          /* fprintf(stderr, "[%u] fastemu idle - timeout %d ms\n", ctx->core, */
          /* 	  timeout_us == (uint32_t)-1 ? -1 : timeout_us / 1000); */
          struct rte_epoll_event event[2];
          int n = rte_epoll_wait(RTE_EPOLL_PER_THREAD, event, 2,
              timeout_us == (uint32_t)-1 ? -1 : timeout_us / 1000);
          assert(n != -1);
          /* fprintf(stderr, "[%u] fastemu busy - %u events\n", ctx->core, n); */
          for(int i = 0; i < n; i++) {
            if(event[i].fd == ctx->evfd) {
              /* fprintf(stderr, "[%u] fastemu - woken up by event FD = %d\n", */
              /* 	      ctx->core, event[i].fd); */
              uint64_t val;
              int r = read(ctx->evfd, &val, sizeof(uint64_t));
              assert(r == sizeof(uint64_t));
            /* } else { */
            /*   fprintf(stderr, "[%u] fastemu - woken up by RX interrupt FD = %d\n", */
            /* 	      ctx->core, event[i].fd); */
            }
          }

          /*fprintf(stderr, "dataplane_loop: woke up %u n=%u fd=%d evfd=%d\n", ctx->id, n, event[0].fd, ctx->evfd);*/
          network_rx_interrupt_ctl(&ctx->net, 0);
        }

        startwait = 0;
      }
    } else {
      was_idle = 0;
      startwait = 0;
    }
  }
}

#ifdef DATAPLANE_STATS
void dataplane_dump_stats(void)
{
  struct dataplane_context *ctx;
  unsigned i;

  for (i = 0; i < fp_cores_max; i++) {
    ctx = ctxs[i];
    if (ctx == NULL)
      continue;

    uint64_t qm_total = STATS_ATOMIC_FETCH(ctx, qm_total);
    uint64_t rx_total = STATS_ATOMIC_FETCH(ctx, rx_total);
    uint64_t qs_total = STATS_ATOMIC_FETCH(ctx, qs_total);
    uint64_t sp_total = STATS_ATOMIC_FETCH(ctx, sp_total);
    uint64_t tx_total = STATS_ATOMIC_FETCH(ctx, tx_total);

    uint64_t cyc_qm = STATS_ATOMIC_FETCH(ctx, cyc_qm);
    uint64_t cyc_qm_useful = STATS_ATOMIC_FETCH(ctx, cyc_qm_useful);
    uint64_t cyc_rx = STATS_ATOMIC_FETCH(ctx, cyc_rx);
    uint64_t cyc_qs = STATS_ATOMIC_FETCH(ctx, cyc_qs);
    uint64_t cyc_qs_useful = STATS_ATOMIC_FETCH(ctx, cyc_qs_useful);
    uint64_t cyc_sp = STATS_ATOMIC_FETCH(ctx, cyc_sp);
    uint64_t cyc_tx = STATS_ATOMIC_FETCH(ctx, cyc_tx);

    uint64_t qm_poll = STATS_ATOMIC_FETCH(ctx, qm_poll);
    uint64_t rx_poll = STATS_ATOMIC_FETCH(ctx, rx_poll);
    uint64_t qs_poll = STATS_ATOMIC_FETCH(ctx, qs_poll);
    uint64_t sp_poll = STATS_ATOMIC_FETCH(ctx, sp_poll);
    uint64_t tx_poll = STATS_ATOMIC_FETCH(ctx, tx_poll);

    uint64_t qm_empty  = STATS_ATOMIC_FETCH(ctx, qm_empty);
    uint64_t rx_empty  = STATS_ATOMIC_FETCH(ctx, rx_empty);
    uint64_t qs_empty  = STATS_ATOMIC_FETCH(ctx, qs_empty);
    uint64_t sp_empty  = STATS_ATOMIC_FETCH(ctx, sp_empty);
    uint64_t tx_empty  = STATS_ATOMIC_FETCH(ctx, tx_empty);

    uint64_t act_timewheel_cnt = STATS_FETCH(&(ctx->qman), act_timewheel_cnt);
    uint64_t queue_new_ts_wrap_cnt = STATS_FETCH(&(ctx->qman), queue_new_ts_wrap_cnt);
    uint64_t timewheel_delta_high = STATS_FETCH(&(ctx->qman), timewheel_delta_high);
    uint64_t cyc_queue_activate = STATS_FETCH(&(ctx->qman), cyc_queue_activate);
    uint64_t cyc_qman_poll = STATS_FETCH(&(ctx->qman), cyc_qman_poll);

    TAS_LOG(INFO, MAIN, "DP [%u]> (POLL, EMPTY, TOTAL, CYC/POLL, CYC/TOTAL, EMPTY/POLL)\n", i);
    TAS_LOG(INFO, MAIN, "qm       =(%"PRIu64",%"PRIu64",%"PRIu64", %lF, %lF, %lF)\n",
            qm_poll, qm_empty,
            qm_total, (double) cyc_qm/qm_poll, (double) cyc_qm/qm_total, (double) qm_empty/qm_poll);
    TAS_LOG(INFO, MAIN, "rx       =(%"PRIu64",%"PRIu64",%"PRIu64", %lF, %lF, %lF)\n",
            rx_poll, rx_empty,
            rx_total, (double) cyc_rx/rx_poll, (double) cyc_rx/rx_total, (double) rx_empty/rx_poll);
    TAS_LOG(INFO, MAIN, "qs       =(%"PRIu64",%"PRIu64",%"PRIu64", %lF, %lF, %lF)\n",
            qs_poll, qs_empty,
            qs_total, (double) cyc_qs/qs_poll, (double) cyc_qs/qs_total, (double) qs_empty/qs_poll);
    TAS_LOG(INFO, MAIN, "sp       =(%"PRIu64",%"PRIu64",%"PRIu64", %lF, %lF, %lF)\n",
            sp_poll, sp_empty,
            sp_total, (double) cyc_sp/sp_poll, (double) cyc_sp/sp_total, (double) sp_empty/sp_poll);
    TAS_LOG(INFO, MAIN, "tx       =(%"PRIu64",%"PRIu64",%"PRIu64", %lF, %lF, %lF)\n",
            tx_poll, tx_empty,
            tx_total, (double) cyc_tx/tx_poll, (double) cyc_tx/tx_total, (double) tx_empty/tx_poll);
    TAS_LOG(INFO, MAIN, "cyc       =(\n\t\t\t\t\t\tcyc_qm = %"PRIu64",\n\t\t\t\t\t\tcyc_qm_useful = %"PRIu64",\n\t\t\t\t\t\tcyc_rx = %"PRIu64",\n\t\t\t\t\t\tcyc_qs = %"PRIu64",\n\t\t\t\t\t\tcyc_qs_useful = %"PRIu64",\n\t\t\t\t\t\tcyc_sp = %"PRIu64",\n\t\t\t\t\t\tcyc_tx = %"PRIu64"\n)\n",
            cyc_qm, cyc_qm_useful, cyc_rx, cyc_qs, cyc_qs_useful, cyc_sp, cyc_tx);
    TAS_LOG(INFO, MAIN, "act_timewheel_cnt=%"PRIu64", queue_new_ts_wrap_cnt=%"PRIu64", timewheel_delta_high=%"PRIu64", cyc_queue_activate=%"PRIu64", cyc_qman_poll=%"PRIu64"\n", act_timewheel_cnt, queue_new_ts_wrap_cnt, timewheel_delta_high, cyc_queue_activate, cyc_qman_poll);

    uint64_t cyc_total = STATS_ATOMIC_FETCH(ctx, cyc_qm) + STATS_ATOMIC_FETCH(ctx, cyc_rx) + STATS_ATOMIC_FETCH(ctx, cyc_qs) + STATS_ATOMIC_FETCH(ctx, cyc_sp) + STATS_ATOMIC_FETCH(ctx, cyc_tx) + 1;
    TAS_LOG(INFO, MAIN, "ratio=(%lf, %lf, %lf, %lf, %lf) \n",
            (double) STATS_ATOMIC_FETCH(ctx, cyc_qm)/cyc_total,
            (double) STATS_ATOMIC_FETCH(ctx, cyc_rx)/cyc_total,
            (double) STATS_ATOMIC_FETCH(ctx, cyc_qs)/cyc_total,
            (double) STATS_ATOMIC_FETCH(ctx, cyc_sp)/cyc_total,
            (double) STATS_ATOMIC_FETCH(ctx, cyc_tx)/cyc_total);

#ifdef QUEUE_STATS
    TAS_LOG(INFO, MAIN, "slow -> fast (%"PRIu64",%"PRIu64") avg_queuing_delay=%lF\n", 
            STATS_ATOMIC_FETCH(ctx, kin_cycles),
            STATS_ATOMIC_FETCH(ctx, kin_count),
            ((double) STATS_ATOMIC_FETCH(ctx, kin_cycles))/ STATS_ATOMIC_FETCH(ctx, kin_count));
#endif
  }
}
#else
void dataplane_dump_stats(void)
{
  return;
}
#endif

static unsigned poll_rx(struct dataplane_context *ctx, uint32_t ts)
{
  int ret;
  unsigned i, n;
  uint8_t freebuf[BATCH_SIZE] = { 0 };
  void *fss[BATCH_SIZE];
  struct tcp_opts tcpopts[BATCH_SIZE];
  struct network_buf_handle *bhs[BATCH_SIZE];

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < n)
    n = TXBUF_SIZE - ctx->tx_num;

  STATS_ADD(ctx, rx_poll, 1);

  /* receive packets */
  ret = network_poll(&ctx->net, n, bhs);
  if (ret <= 0) {
    STATS_ADD(ctx, rx_empty, 1);
    return 0;
  }
  STATS_ADD(ctx, rx_total, n);
  n = ret;

  /* prefetch packet contents (1st cache line) */
  for (i = 0; i < n; i++) {
    rte_prefetch0(network_buf_bufoff(bhs[i]));
  }

  /* look up flow states */
  fast_flows_packet_fss(ctx, bhs, fss, n);

  /* prefetch packet contents (2nd cache line, TS opt overlaps) */
  for (i = 0; i < n; i++) {
    rte_prefetch0(network_buf_bufoff(bhs[i]) + 64);
  }

  /* parse packets */
  fast_flows_packet_parse(ctx, bhs, fss, tcpopts, n);

  for (i = 0; i < n; i++) {
    /* run fast-path for flows with flow state */
    if (fss[i] != NULL) {
      ret = fast_flows_packet(ctx, bhs[i], fss[i], &tcpopts[i], ts);
    } else {
      ret = -1;
    }

    if (ret > 0) {
      freebuf[i] = 1;
    } else if (ret < 0) {
      fast_kernel_packet(ctx, bhs[i]);
    }
  }

  arx_cache_flush(ctx, ts);

  /* free received buffers */
  for (i = 0; i < n; i++) {
    if (freebuf[i] == 0)
      bufcache_free(ctx, bhs[i]);
  }

  return n;
}

static unsigned poll_queues(struct dataplane_context *ctx, uint32_t ts)
{
  struct network_buf_handle **handles;
  void *aqes[BATCH_SIZE];
  unsigned n, i, total = 0;
  uint16_t max, k = 0, num_bufs = 0, j;
  int ret;

  STATS_ADD(ctx, qs_poll, 1);

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM; n++) {
    fast_appctx_poll_pf(ctx, (ctx->poll_next_ctx + n) % FLEXNIC_PL_APPCTX_NUM);
  }

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM && k < max; n++) {
    for (i = 0; i < BATCH_SIZE && k < max; i++) {
      ret = fast_appctx_poll_fetch(ctx, ctx->poll_next_ctx, &aqes[k]);
      if (ret == 0)
        k++;
      else
        break;

      total++;
    }

    ctx->poll_next_ctx = (ctx->poll_next_ctx + 1) %
      FLEXNIC_PL_APPCTX_NUM;
  }

  for (j = 0; j < k; j++) {
    ret = fast_appctx_poll_bump(ctx, aqes[j], handles[num_bufs], ts);
    if (ret == 0)
      num_bufs++;
  }

  /* apply buffer reservations */
  bufcache_alloc(ctx, num_bufs);

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM; n++)
    fast_actx_rxq_probe(ctx, n);

  STATS_ADD(ctx, qs_total, total);
  if (total == 0)
    STATS_ADD(ctx, qs_empty, 1);

  return total;
}

static unsigned poll_kernel(struct dataplane_context *ctx, uint32_t ts)
{
  struct network_buf_handle **handles;
  unsigned total = 0;
  uint16_t max, k = 0;
  int ret;

  STATS_ADD(ctx, sp_poll, 1);

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  max = (max > 8 ? 8 : max);
  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  for (k = 0; k < max;) {
    ret = fast_kernel_poll(ctx, handles[k], ts);

    if (ret == 0)
      k++;
    else if (ret < 0)
      break;

    total++;
  }

  /* apply buffer reservations */
  bufcache_alloc(ctx, k);

  STATS_ADD(ctx, sp_total, total);
  if (total == 0)
    STATS_ADD(ctx, sp_empty, 1);

  return total;
}

static unsigned poll_qman(struct dataplane_context *ctx, uint32_t ts)
{
  unsigned q_ids[BATCH_SIZE];
  uint16_t q_bytes[BATCH_SIZE];
  struct network_buf_handle **handles;
  uint16_t off = 0, max;
  int ret, i, use;

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  STATS_ADD(ctx, qm_poll, 1);

  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  //STATS_TS(start_qman_poll);
  /* poll queue manager */
  ret = qman_poll(&ctx->qman, max, q_ids, q_bytes);
  //STATS_TS(end_qman_poll);

  if (ret <= 0) {
    STATS_ADD(ctx, qm_empty, 1);
    return 0;
  }

  //TAS_LOG(ERR, MAIN, "poll_qman: Send %u bytes for flow_id=%u\n", q_bytes[0], q_ids[0]);

  STATS_ADD(ctx, qm_total, ret);

  for (i = 0; i < ret; i++) {
    rte_prefetch0(handles[i]);
  }

  for (i = 0; i < ret; i++) {
    rte_prefetch0((uint8_t *) handles[i] + 64);
  }

  /* prefetch packet contents */
  for (i = 0; i < ret; i++) {
    rte_prefetch0(network_buf_buf(handles[i]));
  }

  fast_flows_qman_pf(ctx, q_ids, ret);

  fast_flows_qman_pfbufs(ctx, q_ids, ret);

  for (i = 0; i < ret; i++) {
    //STATS_TS(start_fast_flows_qman);
    use = fast_flows_qman(ctx, q_ids[i], handles[off], ts);
    //STATS_TS(end_fast_flows_qman);
    //STATS_ATOMIC_ADD(ctx, cyc_fast_flows_qman, );

    //if (use != 0)
    //  fprintf(stderr, "Didn't send anything!!!\n");
    if (use == 0)
     off++;
  }

  /* apply buffer reservations */
  bufcache_alloc(ctx, off);

  return ret;
}

static unsigned poll_qman_fwd(struct dataplane_context *ctx, uint32_t ts)
{
  void *flow_states[4 * BATCH_SIZE];
  int ret, i;

  /* poll queue manager forwarding ring */
  ret = rte_ring_dequeue_burst(ctx->qman_fwd_ring, flow_states, 4 * BATCH_SIZE, NULL);
  for (i = 0; i < ret; i++) {
    fast_flows_qman_fwd(ctx, flow_states[i]);
  }

  return ret;
}

static inline uint8_t bufcache_prealloc(struct dataplane_context *ctx, uint16_t num,
    struct network_buf_handle ***handles)
{
  uint16_t grow, res, head, g, i;
  struct network_buf_handle *nbh;

  /* try refilling buffer cache */
  if (ctx->bufcache_num < num) {
    grow = BUFCACHE_SIZE - ctx->bufcache_num;
    head = (ctx->bufcache_head + ctx->bufcache_num) & (BUFCACHE_SIZE - 1);

    if (head + grow <= BUFCACHE_SIZE) {
      res = network_buf_alloc(&ctx->net, grow, ctx->bufcache_handles + head);
    } else {
      g = BUFCACHE_SIZE - head;
      res = network_buf_alloc(&ctx->net, g, ctx->bufcache_handles + head);
      if (res == g) {
        res += network_buf_alloc(&ctx->net, grow - g, ctx->bufcache_handles);
      }
    }

    for (i = 0; i < res; i++) {
      g = (head + i) & (BUFCACHE_SIZE - 1);
      nbh = ctx->bufcache_handles[g];
      ctx->bufcache_handles[g] = (struct network_buf_handle *)
        ((uintptr_t) nbh);
    }

    ctx->bufcache_num += res;
  }
  num = MIN(num, (ctx->bufcache_head + ctx->bufcache_num <= BUFCACHE_SIZE ?
        ctx->bufcache_num : BUFCACHE_SIZE - ctx->bufcache_head));

  *handles = ctx->bufcache_handles + ctx->bufcache_head;

  return num;
}

static inline void bufcache_alloc(struct dataplane_context *ctx, uint16_t num)
{
  assert(num <= ctx->bufcache_num);

  ctx->bufcache_head = (ctx->bufcache_head + num) & (BUFCACHE_SIZE - 1);
  ctx->bufcache_num -= num;
}

static inline void bufcache_free(struct dataplane_context *ctx,
    struct network_buf_handle *handle)
{
  uint32_t head, num;

  num = ctx->bufcache_num;
  if (num < BUFCACHE_SIZE) {
    /* free to cache */
    head = (ctx->bufcache_head + num) & (BUFCACHE_SIZE - 1);
    ctx->bufcache_handles[head] = handle;
    ctx->bufcache_num = num + 1;
    network_buf_reset(handle);
  } else {
    /* free to network buffer manager */
    network_free(1, &handle);
  }
}

static inline void tx_flush(struct dataplane_context *ctx)
{
  int ret;
  unsigned i;

  if (ctx->tx_num == 0) {
    return;
  }

  STATS_ATOMIC_ADD(ctx, tx_poll, 1);

  /* try to send out packets */
  ret = network_send(&ctx->net, ctx->tx_num, ctx->tx_handles);

  if (ret == ctx->tx_num) {
    /* everything sent */
    ctx->tx_num = 0;
  } else if (ret > 0) {
    /* move unsent packets to front */
    for (i = ret; i < ctx->tx_num; i++) {
      ctx->tx_handles[i - ret] = ctx->tx_handles[i];
    }
    ctx->tx_num -= ret;
  }

  STATS_ATOMIC_ADD(ctx, tx_total, ret);
  if (ret == 0)
    STATS_ATOMIC_ADD(ctx, tx_empty, 1);
}

static void poll_scale(struct dataplane_context *ctx)
{
  unsigned st = fp_scale_to;

  if (st == 0)
    return;

  fprintf(stderr, "Scaling fast path from %u to %u\n", fp_cores_cur, st);
  if (st < fp_cores_cur) {
    if (network_scale_down(fp_cores_cur, st) != 0) {
      fprintf(stderr, "network_scale_down failed\n");
      abort();
    }
  } else if (st > fp_cores_cur) {
    if (network_scale_up(fp_cores_cur, st) != 0) {
      fprintf(stderr, "network_scale_up failed\n");
      abort();
    }
  } else {
    fprintf(stderr, "poll_scale: warning core number didn't change\n");
  }

  fp_cores_cur = st;
  fp_scale_to = 0;
}

static void arx_cache_flush(struct dataplane_context *ctx, uint32_t ts)
{
  uint16_t i;
  struct flextcp_pl_appctx *actx;
  struct flextcp_pl_arx *parx[BATCH_SIZE];

  for (i = 0; i < ctx->arx_num; i++) {
    actx = &fp_state->appctx[ctx->id][ctx->arx_ctx[i]];
    if (fast_actx_rxq_alloc(ctx, actx, &parx[i]) != 0) {
      /* TODO: how do we handle this? */
      fprintf(stderr, "arx_cache_flush: no space in app rx queue\n");
      abort();
    }
  }

  for (i = 0; i < ctx->arx_num; i++) {
    rte_prefetch0(parx[i]);
  }

  for (i = 0; i < ctx->arx_num; i++) {
    *parx[i] = ctx->arx_cache[i];
  }

  for (i = 0; i < ctx->arx_num; i++) {
    actx = &fp_state->appctx[ctx->id][ctx->arx_ctx[i]];
    actx_kick(actx, ts);
  }

  ctx->arx_num = 0;
}
