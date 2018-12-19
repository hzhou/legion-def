#define CHECK_PTHREAD(cmd) do { \
  int ret = (cmd); \
  if(ret != 0) { \
    fprintf(stderr, "PTHREAD: %s = %d (%s)\n", #cmd, ret, strerror(ret)); \
    exit(1); \
  } \
} while(0)

#define CHECK_GASNET(cmd) do { \
  int ret = (cmd); \
  if(ret != GASNET_OK) { \
    fprintf(stderr, "GASNET: %s = %d (%s, %s)\n", #cmd, ret, gasnet_ErrorName(ret), gasnet_ErrorDesc(ret)); \
    exit(1); \
  } \
} while(0)

#ifdef REALM_PROFILE_AM_HANDLERS
struct ActiveMsgHandlerStats {
  size_t count, sum, sum2, minval, maxval;

  ActiveMsgHandlerStats(void)
  : count(0), sum(0), sum2(0), minval(0), maxval(0) {}

  void record(struct timespec& ts_start, struct timespec& ts_end)
  {
    size_t val = 1000000000LL * (ts_end.tv_sec - ts_start.tv_sec) + ts_end.tv_nsec - ts_start.tv_nsec;
    if(!count || (val < minval)) minval = val;
    if(!count || (val > maxval)) maxval = val;
    count++;
    sum += val;
    sum2 += val * val;
  }
};

static ActiveMsgHandlerStats handler_stats[256];

void record_activemsg_profiling(int msgid,
				const struct timespec& ts_start,
				const struct timespec& ts_end)
{
  handler_stats[msgid].record(ts_start, ts_end);
}
#endif

NodeID get_message_source(token_t token)
{
  gasnet_node_t src;
  CHECK_GASNET( gasnet_AMGetMsgSource(reinterpret_cast<gasnet_token_t>(token), &src) );
#ifdef DEBUG_AMREQUESTS
  printf("%d: source = %d\n", my_node_id, src);
#endif
  return src;
}  

void send_srcptr_release(token_t token, uint64_t srcptr)
{
  CHECK_GASNET( gasnet_AMReplyShort2(reinterpret_cast<gasnet_token_t>(token), MSGID_RELEASE_SRCPTR, (handlerarg_t)srcptr, (handlerarg_t)(srcptr >> 32)) );
}

#ifdef DEBUG_MEM_REUSE
static int payload_count = 0;
#endif

Realm::Logger log_amsg("activemsg");
Realm::Logger log_spill("spill");

#ifdef ACTIVE_MESSAGE_TRACE
Realm::Logger log_amsg_trace("amtrace");

void record_am_handler(int handler_id, const char *description, bool reply)
{
  log_amsg_trace.info("AM Handler: %d %s %s", handler_id, description,
		      (reply ? "Reply" : "Request"));
}
#endif

#ifdef REALM_PROFILE_AM_HANDLERS
/*extern*/ ActiveMsgHandlerStats handler_stats[256];
#endif

static const int DEFERRED_FREE_COUNT = 128;
$call hsl_decl, deferred_free_mutex
int deferred_free_pos;
void *deferred_frees[DEFERRED_FREE_COUNT];

gasnet_seginfo_t *segment_info = 0;

static bool is_registered(void *ptr)
{
  ssize_t offset = ((char *)ptr) - ((char *)(segment_info[my_node_id].addr));
  if((offset >= 0) && ((size_t)offset < segment_info[my_node_id].size))
    return true;
  return false;
}

$call class_IncomingMessageManager

void init_deferred_frees(void)
{
  $call hsl_mutex, init, deferred_free_mutex
  deferred_free_pos = 0;
  for(int i = 0; i < DEFERRED_FREE_COUNT; i++)
    deferred_frees[i] = 0;
}

void deferred_free(void *ptr)
{
#ifdef DEBUG_MEM_REUSE
  printf("%d: deferring free of %p\n", my_node_id, ptr);
#endif
  $call hsl_mutex, lock, deferred_free_mutex
  void *oldptr = deferred_frees[deferred_free_pos];
  deferred_frees[deferred_free_pos] = ptr;
  deferred_free_pos = (deferred_free_pos + 1) % DEFERRED_FREE_COUNT;
  $call hsl_mutex, unlock, deferred_free_mutex
  if(oldptr) {
#ifdef DEBUG_MEM_REUSE
    printf("%d: actual free of %p\n", my_node_id, oldptr);
#endif
    free(oldptr);
  }
}

$call class_Payload

$call struct_OutgoingMessage

Realm::Logger log_sdp("srcdatapool");

$call class_SrcDataPool
// templates/activemsg_dead_1.cc

$call impl_OutgoingMessage
// these values can be overridden by command-line parameters
static int num_lmbs = 2;
static size_t lmb_size = 1 << 20; // 1 MB
static bool force_long_messages = true;
static int max_msgs_to_send = 8;

// returns the largest payload that can be sent to a node (to a non-pinned
//   address)
size_t get_lmb_size(NodeID target_node)
{
  // not node specific right yet
  return lmb_size;
}

#ifdef DETAILED_MESSAGE_TIMING
$call class_DetailedMessageTiming

static DetailedMessageTiming detailed_message_timing;
#endif

$call impl_IncomingMessageManager

static IncomingMessageManager *incoming_message_manager = 0;

extern void enqueue_incoming(NodeID sender, IncomingMessage *msg)
{
#ifdef DEBUG_AMREQUESTS
  printf("%d: incoming(%d, %p)\n", my_node_id, sender, msg);
#endif
  assert(incoming_message_manager != 0);
  incoming_message_manager->add_incoming_message(sender, msg);
}


$call class_ActiveMessageEndpoint

$call class_EndpointManager
static EndpointManager *endpoint_manager;

static void handle_flip_req(gasnet_token_t token,
		     int flip_buffer, int flip_count)
{
  gasnet_node_t src;
  CHECK_GASNET( gasnet_AMGetMsgSource(token, &src) );
  endpoint_manager->handle_flip_request(src, flip_buffer, flip_count);
}

static void handle_flip_ack(gasnet_token_t token,
			    int ack_buffer)
{
  gasnet_node_t src;
  CHECK_GASNET( gasnet_AMGetMsgSource(token, &src) );
  endpoint_manager->handle_flip_ack(src, ack_buffer);
}

static const int MAX_HANDLERS = 128;
static gasnet_handlerentry_t handlers[MAX_HANDLERS];
static int hcount = 0;

void add_handler_entry(int msgid, void (*fnptr)())
{
  assert(hcount < MAX_HANDLERS);
  handlers[hcount].index = msgid;
  handlers[hcount].fnptr = fnptr;
  hcount++;
}
			   
void init_endpoints(int gasnet_mem_size_in_mb,
		    int registered_mem_size_in_mb,
		    int registered_ib_mem_size_in_mb,
		    Realm::CoreReservationSet& crs,
		    std::vector<std::string>& cmdline)
{
  size_t lmbsize_in_kb = 0;
  size_t sdpsize_in_mb = 64;
  size_t spillwarn_in_mb = 0;
  size_t spillstep_in_mb = 0;
  size_t spillstall_in_mb = 0;

  Realm::CommandLineParser cp;
  cp.add_option_int("-ll:numlmbs", num_lmbs)
    .add_option_int("-ll:lmbsize", lmbsize_in_kb)
    .add_option_int("-ll:forcelong", force_long_messages)
    .add_option_int("-ll:sdpsize", sdpsize_in_mb)
    .add_option_int("-ll:maxsend", max_msgs_to_send)
    .add_option_int("-ll:spillwarn", spillwarn_in_mb)
    .add_option_int("-ll:spillstep", spillstep_in_mb)
    .add_option_int("-ll:spillstall", spillstep_in_mb);

  bool ok = cp.parse_command_line(cmdline);
  assert(ok);

  size_t srcdatapool_size = sdpsize_in_mb << 20;
  if(lmbsize_in_kb) lmb_size = lmbsize_in_kb << 10;
  if(spillwarn_in_mb)
    SrcDataPool::print_spill_threshold = spillwarn_in_mb << 20;
  if(spillstep_in_mb)
    SrcDataPool::print_spill_step = spillstep_in_mb << 20;
  if(spillstall_in_mb)
    SrcDataPool::max_spill_bytes = spillstall_in_mb << 20;

  size_t total_lmb_size = ((max_node_id+1) * 
			   num_lmbs *
			   lmb_size);

  // add in our internal handlers and space we need for LMBs
  size_t attach_size = ((((size_t)gasnet_mem_size_in_mb) << 20) +
			(((size_t)registered_mem_size_in_mb) << 20) +
			(((size_t)registered_ib_mem_size_in_mb) << 20) +
			srcdatapool_size +
			total_lmb_size);

  if(my_node_id == 0) {
    log_amsg.info("Pinned Memory Usage: GASNET=%d, RMEM=%d, IBRMEM=%d, LMB=%zd, SDP=%zd, total=%zd\n",
		  gasnet_mem_size_in_mb, registered_mem_size_in_mb, registered_ib_mem_size_in_mb,
		  total_lmb_size >> 20, srcdatapool_size >> 20,
		  attach_size >> 20);
#ifdef DEBUG_REALM_STARTUP
    Realm::TimeStamp ts("entering gasnet_attach", false);
    fflush(stdout);
#endif
  }

  // Don't bother checking this here.  Some GASNet conduits lie if 
  // the GASNET_PHYSMEM_MAX variable is not set.
#if 0
  if (attach_size > gasnet_getMaxLocalSegmentSize())
  {
    fprintf(stderr,"ERROR: Legion exceeded maximum GASNet segment size. "
                   "Requested %ld bytes but maximum set by GASNET "
                   "configuration is %ld bytes.  Legion will now exit...",
                   attach_size, gasnet_getMaxLocalSegmentSize());
    assert(false);
  }
#endif

  assert(hcount < (MAX_HANDLERS - 3));
  handlers[hcount].index = MSGID_FLIP_REQ;
  handlers[hcount].fnptr = (void (*)())handle_flip_req;
  hcount++;
  handlers[hcount].index = MSGID_FLIP_ACK;
  handlers[hcount].fnptr = (void (*)())handle_flip_ack;
  hcount++;
  handlers[hcount].index = MSGID_RELEASE_SRCPTR;
  handlers[hcount].fnptr = (void (*)())SrcDataPool::release_srcptr_handler;
  hcount++;
#ifdef ACTIVE_MESSAGE_TRACE
  record_am_handler(MSGID_FLIP_REQ, "Flip Request AM");
  record_am_handler(MSGID_FLIP_ACK, "Flip Acknowledgement AM");
  record_am_handler(MSGID_RELEASE_SRCPTR, "Release Source Pointer AM");
#endif

  CHECK_GASNET( gasnet_attach(handlers, hcount,
			      attach_size, 0) );

#ifdef DEBUG_REALM_STARTUP
  if(my_node_id == 0) {
    Realm::TimeStamp ts("exited gasnet_attach", false);
    fflush(stdout);
  }
#endif

  // once we've attached, attempt to synchronize all node's clocks
  $call barrier
  $call barrier
  Realm::Clock::set_zero_time();
  $call barrier
  
  segment_info = new gasnet_seginfo_t[max_node_id+1];
  CHECK_GASNET( gasnet_getSegmentInfo(segment_info, max_node_id+1) );

  char *my_segment = (char *)(segment_info[my_node_id].addr);
  /*char *gasnet_mem_base = my_segment;*/  my_segment += (gasnet_mem_size_in_mb << 20);
  /*char *reg_mem_base = my_segment;*/  my_segment += (registered_mem_size_in_mb << 20);
  /*char *reg_ib_mem_base = my_segment;*/ my_segment += (registered_ib_mem_size_in_mb << 20);
  char *srcdatapool_base = my_segment;  my_segment += srcdatapool_size;
  /*char *lmb_base = my_segment;*/  my_segment += total_lmb_size;
  assert(my_segment <= ((char *)(segment_info[my_node_id].addr) + segment_info[my_node_id].size)); 

#ifndef NO_SRCDATAPOOL
  if(srcdatapool_size > 0)
    srcdatapool = new SrcDataPool(srcdatapool_base, srcdatapool_size);
#endif

  endpoint_manager = new EndpointManager(max_node_id+1, crs);

  init_deferred_frees();
}

// do a little bit of polling to try to move messages along, but return
//  to the caller rather than spinning
void do_some_polling(void)
{
  endpoint_manager->push_messages(max_msgs_to_send);

  CHECK_GASNET( gasnet_AMPoll() );
}

void start_polling_threads(int count)
{
  endpoint_manager->start_polling_threads(count);
}

void start_handler_threads(int count, Realm::CoreReservationSet& crs, size_t stack_size)
{
  incoming_message_manager = new IncomingMessageManager(max_node_id+1, crs);

  incoming_message_manager->start_handler_threads(count, stack_size);
}

void stop_activemsg_threads(void)
{
  endpoint_manager->stop_threads();
	
  incoming_message_manager->shutdown();
  delete incoming_message_manager;

#ifdef REALM_PROFILE_AM_HANDLERS
  for(int i = 0; i < 256; i++) {
    if(!handler_stats[i].count) continue;
    double avg = ((double)handler_stats[i].sum) / ((double)handler_stats[i].count);
    double stddev = sqrt((((double)handler_stats[i].sum2) / ((double)handler_stats[i].count)) -
                         avg * avg);
    printf("AM profiling: node %d, msg %d: count = %10zd, avg = %8.2f, dev = %8.2f, min = %8zd, max = %8zd\n",
           my_node_id, i,
           handler_stats[i].count, avg, stddev, handler_stats[i].minval, handler_stats[i].maxval);
  }
#endif

#ifdef DETAILED_MESSAGE_TIMING
  // dump timing data from all the endpoints to a file
  detailed_message_timing.dump_detailed_timing_data();
#endif

  // print final spill stats at a low logging level
  srcdatapool->print_spill_data(Realm::Logger::LEVEL_INFO);
}
	
void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const void *payload, size_t payload_size,
		     int payload_mode, void *dstptr)
{
  assert((gasnet_node_t)target != my_node_id);

  OutgoingMessage *hdr = new OutgoingMessage(msgid, 
					     (arg_size + sizeof(int) - 1) / sizeof(int),
					     args);

  // if we have a contiguous payload that is in the KEEP mode, and in
  //  registered memory, we may be able to avoid a copy
  if((payload_mode == PAYLOAD_KEEP) && is_registered((void *)payload))
    payload_mode = PAYLOAD_KEEPREG;

  if (payload_mode != PAYLOAD_NONE)
  {
    if (payload_size > 0) {
      PayloadSource *payload_src = 
        new ContiguousPayload((void *)payload, payload_size, payload_mode);
      hdr->set_payload(payload_src, payload_size, payload_mode, dstptr);
    } else {
      hdr->set_payload_empty();
    }
  }

  endpoint_manager->enqueue_message(target, hdr, true); // TODO: decide when OOO is ok?
}

void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const void *payload, size_t line_size,
		     off_t line_stride, size_t line_count,
		     int payload_mode, void *dstptr)
{
  assert((gasnet_node_t)target != my_node_id);

  OutgoingMessage *hdr = new OutgoingMessage(msgid, 
					     (arg_size + sizeof(int) - 1) / sizeof(int),
					     args);

  if (payload_mode != PAYLOAD_NONE)
  {
    size_t payload_size = line_size * line_count;
    if (payload_size > 0) {
      PayloadSource *payload_src = new TwoDPayload(payload, line_size, 
                                       line_count, line_stride, payload_mode);
      hdr->set_payload(payload_src, payload_size, payload_mode, dstptr);
    } else {
      hdr->set_payload_empty();
    }
  }
  endpoint_manager->enqueue_message(target, hdr, true); // TODO: decide when OOO is ok?
}

void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const SpanList& spans, size_t payload_size,
		     int payload_mode, void *dstptr)
{
  assert((gasnet_node_t)target != my_node_id);

  OutgoingMessage *hdr = new OutgoingMessage(msgid, 
  					     (arg_size + sizeof(int) - 1) / sizeof(int),
  					     args);

  if (payload_mode != PAYLOAD_NONE)
  {
    if (payload_size > 0) {
      PayloadSource *payload_src = new SpanPayload(spans, payload_size, payload_mode);
      hdr->set_payload(payload_src, payload_size, payload_mode, dstptr);
    } else {
      hdr->set_payload_empty();
    }
  }

  endpoint_manager->enqueue_message(target, hdr, true); // TODO: decide when OOO is ok?
}

void handle_long_msgptr(NodeID source, const void *ptr)
{
  assert((gasnet_node_t)source != my_node_id);

  endpoint_manager->handle_long_msgptr(source, ptr);
}

/*static*/ void SrcDataPool::release_srcptr_handler(gasnet_token_t token,
						    gasnet_handlerarg_t arg0,
						    gasnet_handlerarg_t arg1)
{
  uintptr_t srcptr = (((uint64_t)(uint32_t)arg1) << 32) | ((uint32_t)arg0);
  // We may get pointers which are zero because we had to send a reply
  // Just ignore them
  if (srcptr != 0)
    srcdatapool->release_srcptr((void *)srcptr);
#ifdef TRACE_MESSAGES
  gasnet_node_t src;
  CHECK_GASNET( gasnet_AMGetMsgSource(token, &src) );
  endpoint_manager->record_message(src, false/*sent reply*/);
#endif
}

extern bool adjust_long_msgsize(NodeID source, void *&ptr, size_t &buffer_size,
				int message_id, int chunks)
{
  // special case: if the buffer size is zero, it's an empty message and no adjustment
  //   is needed
  if(buffer_size == 0)
    return true;

  assert((gasnet_node_t)source != my_node_id);

  return endpoint_manager->adjust_long_msgsize(source, ptr, buffer_size,
					       message_id, chunks);
}

extern void report_activemsg_status(FILE *f)
{
  endpoint_manager->report_activemsg_status(f); 
}

extern void record_message(NodeID source, bool sent_reply)
{
#ifdef TRACE_MESSAGES
  endpoint_manager->record_message(source, sent_reply);
#endif
}

