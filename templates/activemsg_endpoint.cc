// Hui: change cond -> condvar for consistency
class ActiveMessageEndpoint {
public:
  struct ChunkInfo {
  public:
    ChunkInfo(void) : base_ptr(NULL), chunks(0), total_size(0) { }
    ChunkInfo(void *base, int c, size_t size)
      : base_ptr(base), chunks(c), total_size(size) { }
  public:
    void *base_ptr;
    int chunks;
    size_t total_size;
  };
public:
  ActiveMessageEndpoint(gasnet_node_t _peer)
    : peer(_peer)
  {
    $call AM_get_maxes
    $call hsl_, init
    $call cond_, init

    cur_write_lmb = 0;
    cur_write_offset = 0;
    cur_write_count = 0;

    //cur_long_ptr = 0;
    //cur_long_chunk_idx = 0;
    //cur_long_size = 0;
    next_outgoing_message_id = 0;

    lmb_w_bases = new char *[num_lmbs];
    lmb_r_bases = new char *[num_lmbs];
    lmb_r_counts = new int[num_lmbs];
    lmb_w_avail = new bool[num_lmbs];

    for(int i = 0; i < num_lmbs; i++) {
      lmb_w_bases[i] = ((char *)(segment_info[peer].addr)) + (segment_info[peer].size - lmb_size * (my_node_id * num_lmbs + i + 1));
      lmb_r_bases[i] = ((char *)(segment_info[my_node_id].addr)) + (segment_info[peer].size - lmb_size * (peer * num_lmbs + i + 1));
      lmb_r_counts[i] = 0;
      lmb_w_avail[i] = true;
    }
#ifdef TRACE_MESSAGES
    sent_messages = 0;
    received_messages = 0;
#endif
#ifdef DETAILED_MESSAGE_TIMING
    message_log_state = MSGLOGSTATE_NORMAL;
#endif
  }

  ~ActiveMessageEndpoint(void)
  {
    delete[] lmb_w_bases;
    delete[] lmb_r_bases;
    delete[] lmb_r_counts;
    delete[] lmb_w_avail;
  }

  void record_message(bool sent_reply) 
  {
#ifdef TRACE_MESSAGES
    __sync_fetch_and_add(&received_messages, 1);
    if (sent_reply)
      __sync_fetch_and_add(&sent_messages, 1);
#endif
  }

  // returns true if the limit was reached before all messages could
  //  be sent (i.e. there's still more to send)
  bool push_messages(int max_to_send = 0, bool wait = false)
  {
    int count = 0;

    bool still_more = true;
    while(still_more && ((max_to_send == 0) || (count < max_to_send))) {
      // attempt to get the mutex that covers the outbound queues - do not
      //  block
      $call trylock_, break

      // short messages are used primarily for flow control, so always try to send those first
      // (if a short message needs to be ordered with long messages, it goes in the long message
      // queue)
      if(out_short_hdrs.size() > 0) {
	OutgoingMessage *hdr = out_short_hdrs.front();
	out_short_hdrs.pop();
	still_more = !(out_short_hdrs.empty() && out_long_hdrs.empty());

#ifdef DETAILED_MESSAGE_TIMING
	int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
	unsigned qdepth = out_long_hdrs.size(); // sic - we assume the short queue is always near-empty
	message_log_state = MSGLOGSTATE_NORMAL;
#endif
	// now let go of lock and send message
        $call hsl_, unlock

#ifdef DETAILED_MESSAGE_TIMING
	CurrentTime start_time;
#endif
	send_short(hdr);
#ifdef DETAILED_MESSAGE_TIMING
	detailed_message_timing.record(timing_idx, peer, hdr->msgid, -1, hdr->num_args*4, qdepth, start_time, CurrentTime());
#endif
	delete hdr;
	count++;
	continue;
      }

      $call push_messages_long
      // Couldn't do anything so if we were told to wait, goto sleep
      if (wait)
      {
        $call cond_, wait
      }
      // if we get here, we didn't find anything to do, so break out of loop
      //  after releasing the lock
      $call hsl_, unlock
      break;
    }

    return still_more;
  }

  bool enqueue_message(OutgoingMessage *hdr, bool in_order)
  {
    // BEFORE we take the message manager's mutex, we reserve the ability to
    //  allocate spill data
    if((hdr->payload_size > 0) &&
       ((hdr->payload_mode == PAYLOAD_COPY) ||
	(hdr->payload_mode == PAYLOAD_FREE))) {
      SrcDataPool::Lock held_lock(*srcdatapool);
      bool first_try = true;
      while(!srcdatapool->alloc_spill_memory(hdr->payload_size,
					     hdr->msgid,
					     held_lock,
					     first_try)) {
	log_spill.debug() << "spill reservation failed - retrying...";
	first_try = false;
      }
    }

    // need to hold the mutex in order to push onto one of the queues
    $call hsl_, lock

    // once we have the lock, we can safely move the message's payload to
    //  srcdatapool
    hdr->reserve_srcdata();

    // tell caller if we were empty before for managing todo lists
    bool was_empty = out_short_hdrs.empty() && out_long_hdrs.empty();

    // messages that don't need space in the LMB can progress when the LMB is full
    //  (unless they need to maintain ordering with long packets)
    $call enqueue_messages
    // Signal in case there is a sleeping sender
    $call cond_, signal

    $call hsl_, unlock

    return was_empty;
  }

  // returns true if a message is enqueue AND we were empty before
  bool handle_long_msgptr(const void *ptr)
  {
    // can figure out which buffer it is without holding lock
    int r_buffer = -1;
    for(int i = 0; i < num_lmbs; i++)
      if((ptr >= lmb_r_bases[i]) && (ptr < (lmb_r_bases[i] + lmb_size))) {
      r_buffer = i;
      break;
    }
    if(r_buffer < 0) {
      // probably a medium message?
      return false;
    }
    //assert(r_buffer >= 0);

#ifdef DEBUG_LMB
    printf("LMB: received %p for %d->%d in buffer %d, [%p, %p)\n",
	   ptr, peer, my_node_id, r_buffer, lmb_r_bases[r_buffer],
	   lmb_r_bases[r_buffer] + lmb_size);
#endif

    // now take the lock to increment the r_count and decide if we need
    //  to ack (can't actually send it here, so queue it up)
    bool message_added_to_empty_queue = false;
    $call hsl_, lock
    lmb_r_counts[r_buffer]++;
    if(lmb_r_counts[r_buffer] == 0) {
#ifdef DEBUG_LMB
      printf("LMB: acking flip of buffer %d for %d->%d, [%p,%p)\n",
	     r_buffer, peer, my_node_id, lmb_r_bases[r_buffer],
	     lmb_r_bases[r_buffer]+lmb_size);
#endif

      message_added_to_empty_queue = out_short_hdrs.empty() && out_long_hdrs.empty();
      OutgoingMessage *hdr = new OutgoingMessage(MSGID_FLIP_ACK, 1, &r_buffer);
      out_short_hdrs.push(hdr);
      // wake up a sender
      $call cond_, signal
    }
    $call hsl_, unlock
    return message_added_to_empty_queue;
  }

  bool adjust_long_msgsize(void *&ptr, size_t &buffer_size, 
                           int message_id, int chunks)
  {
#ifdef DEBUG_AMREQUESTS
    printf("%d: adjust(%p, %zd, %d, %d)\n", my_node_id, ptr, buffer_size, message_id, chunks);
#endif
    // Quick out, if there was only one chunk, then we are good to go
    if (chunks == 1)
      return true;

    bool ready = false;;
    // now we need to hold the lock
    $call hsl_, lock
    // See if we've seen this message id before
    std::map<int,ChunkInfo>::iterator finder = 
      observed_messages.find(message_id);
    if (finder == observed_messages.end())
    {
      // haven't seen it before, mark that we've seen the first chunk
      observed_messages[message_id] = ChunkInfo(ptr, 1, buffer_size);
    }
    else
    {
      // Update the pointer with the smallest one which is the base
      if (((unsigned long)(ptr)) < ((unsigned long)(finder->second.base_ptr)))
        finder->second.base_ptr = ptr;
      finder->second.total_size += buffer_size;
      finder->second.chunks++;
      // See if we've seen the last chunk
      if (finder->second.chunks == chunks)
      {
        // We've seen all the chunks, now update the pointer
        // and the buffer size and mark that we can handle the message
        ptr = finder->second.base_ptr;
        buffer_size = finder->second.total_size;
        ready = true;
        // Remove the entry from the map
        observed_messages.erase(finder);
      }
      // Otherwise we're not done yet
    }
    $call hsl_, unlock
    return ready;
  }

  // called when the remote side tells us that there will be no more
  //  messages sent for a given buffer - as soon as we've received them all,
  //  we can ack
  bool handle_flip_request(int buffer, int count)
  {
#ifdef DEBUG_LMB
    printf("LMB: received flip of buffer %d for %d->%d, [%p,%p), count=%d\n",
	   buffer, peer, my_node_id, lmb_r_bases[buffer],
	   lmb_r_bases[buffer]+lmb_size, count);
#endif
#ifdef TRACE_MESSAGES
    __sync_fetch_and_add(&received_messages, 1);
#endif
    bool message_added_to_empty_queue = false;
    $call hsl_, lock
    lmb_r_counts[buffer] -= count;
    if(lmb_r_counts[buffer] == 0) {
#ifdef DEBUG_LMB
      printf("LMB: acking flip of buffer %d for %d->%d, [%p,%p)\n",
	     buffer, peer, my_node_id, lmb_r_bases[buffer],
	     lmb_r_bases[buffer]+lmb_size);
#endif

      message_added_to_empty_queue = out_short_hdrs.empty() && out_long_hdrs.empty();
      OutgoingMessage *hdr = new OutgoingMessage(MSGID_FLIP_ACK, 1, &buffer);
      out_short_hdrs.push(hdr);
      // Wake up a sender
      $call cond_, signal
    }
    $call hsl_, unlock
    return message_added_to_empty_queue;
  }

  // called when the remote side says it has received all the messages in a
  //  given buffer - we can that mark that write buffer as available again
  //  (don't even need to take the mutex!)
  void handle_flip_ack(int buffer)
  {
#ifdef DEBUG_LMB
    printf("LMB: received flip ack of buffer %d for %d->%d, [%p,%p)\n",
	   buffer, my_node_id, peer, lmb_w_bases[buffer],
	   lmb_w_bases[buffer]+lmb_size);
#endif
#ifdef TRACE_MESSAGES
    __sync_fetch_and_add(&received_messages, 1);
#endif

    lmb_w_avail[buffer] = true;
    // wake up a sender in case we had messages waiting for free space
    $call hsl_, lock
    $call cond_, signal
    $call hsl_, unlock
  }

protected:
  void send_short(OutgoingMessage *hdr)
  {
    Realm::DetailedTimer::ScopedPush sp(TIME_AM);
#ifdef DEBUG_AMREQUESTS
    printf("%d->%d: %s %d %d %p %zd / %x %x %x %x / %x %x %x %x / %x %x %x %x / %x %x %x %x\n",
	   my_node_id, peer, 
	   ((hdr->payload_mode == PAYLOAD_NONE) ? "SHORT" : "MEDIUM"),
	   hdr->num_args, hdr->msgid,
	   hdr->payload, hdr->payload_size,
	   hdr->args[0], hdr->args[1], hdr->args[2],
	   hdr->args[3], hdr->args[4], hdr->args[5],
	   hdr->args[6], hdr->args[7], hdr->args[8],
	   hdr->args[9], hdr->args[10], hdr->args[11],
	   hdr->args[12], hdr->args[13], hdr->args[14], hdr->args[15]);
    fflush(stdout);
#endif
#ifdef TRACE_MESSAGES
    __sync_fetch_and_add(&sent_messages, 1);
#endif
#ifdef ACTIVE_MESSAGE_TRACE
    log_amsg_trace.info("Active Message Request: %d %d %d %ld",
			hdr->msgid, peer, hdr->num_args, 
			(hdr->payload_mode == PAYLOAD_NONE) ? 
			  0 : hdr->payload_size);
#endif
    switch(hdr->num_args) {
        $call send_short_cases
    default:
      fprintf(stderr, "need to support short/medium of size=%d\n", hdr->num_args);
      assert(1==2);
    }
  }
  
  $call send_long

  gasnet_node_t peer;
  size_t max_medium;
  size_t max_long;
  $call hsl_cond_decl
public:
  std::queue<OutgoingMessage *> out_short_hdrs;
  std::queue<OutgoingMessage *> out_long_hdrs;

  int cur_write_lmb, cur_write_count;
  size_t cur_write_offset;
  char **lmb_w_bases; // [num_lmbs]
  char **lmb_r_bases; // [num_lmbs]
  int *lmb_r_counts; // [num_lmbs]
  bool *lmb_w_avail; // [num_lmbs]
  //void *cur_long_ptr;
  //int cur_long_chunk_idx;
  //size_t cur_long_size;
  std::map<int/*message id*/,ChunkInfo> observed_messages;
  int next_outgoing_message_id;
#ifdef TRACE_MESSAGES
  int sent_messages;
  int received_messages;
#endif
#ifdef DETAILED_MESSAGE_TIMING
  int message_log_state;
#endif
};
