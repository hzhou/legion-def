void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const void *payload, size_t payload_size,
		     int payload_mode, void *dstptr)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const void *payload, size_t line_size,
		     off_t line_stride, size_t line_count,
		     int payload_mode, void *dstptr)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

void enqueue_message(NodeID target, int msgid,
		     const void *args, size_t arg_size,
		     const SpanList& spans, size_t payload_size,
		     int payload_mode, void *dstptr)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

void do_some_polling(void)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

size_t get_lmb_size(NodeID target_node)
{
  return 0;
}

void record_message(NodeID source, bool sent_reply)
{
}

void send_srcptr_release(token_t token, uint64_t srcptr)
{
}

NodeID get_message_source(token_t token)
{
  return 0;
}

void enqueue_incoming(NodeID sender, IncomingMessage *msg)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

bool adjust_long_msgsize(NodeID source, void *&ptr, size_t &buffer_size,
			 int message_id, int chunks)
{
  return false;
}

void handle_long_msgptr(NodeID source, const void *ptr)
{
  assert(0 && "compiled without USE_GASNET - active messages not available!");
}

void add_handler_entry(int msgid, void (*fnptr)())
{
  // ignored
}

void init_endpoints(int gasnet_mem_size_in_mb,
		    int registered_mem_size_in_mb,
		    int registered_ib_mem_size_in_mb,
		    Realm::CoreReservationSet& crs,
		    std::vector<std::string>& cmdline)
{
  // nothing to do without GASNet
}

void start_polling_threads(int)
{
}

void start_handler_threads(int, Realm::CoreReservationSet&, size_t)
{
}

void stop_activemsg_threads(void)
{
}

