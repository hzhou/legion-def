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

      // try to send a long message, but only if we have an LMB available
      //  on the receiving end
      if(out_long_hdrs.size() > 0) {
	OutgoingMessage *hdr;
	hdr = out_long_hdrs.front();

	// no payload?  this happens when a short/medium message needs to be ordered with long messages
	if(hdr->payload_size == 0) {
	  out_long_hdrs.pop();
	  still_more = !(out_short_hdrs.empty() && out_long_hdrs.empty());
#ifdef DETAILED_MESSAGE_TIMING
	  int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
	  unsigned qdepth = out_long_hdrs.size();
	  message_log_state = MSGLOGSTATE_NORMAL;
#endif
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

	// is the message still waiting on space in the srcdatapool?
	if(hdr->payload_mode == PAYLOAD_PENDING) {
#ifdef DETAILED_MESSAGE_TIMING
	  // log this if we haven't already
	  int timing_idx = -1;
	  unsigned qdepth = out_long_hdrs.size();
	  if(message_log_state != MSGLOGSTATE_SRCDATAWAIT) {
	    timing_idx = detailed_message_timing.get_next_index();
	    message_log_state = MSGLOGSTATE_SRCDATAWAIT;
	  }
#endif
          $call hsl_, unlock
#ifdef DETAILED_MESSAGE_TIMING
	  CurrentTime now;
	  detailed_message_timing.record(timing_idx, peer, hdr->msgid, -2, hdr->num_args*4 + hdr->payload_size, qdepth, now, now);
#endif
	  break;
	}

	// do we have a known destination pointer on the target?  if so, no need to use LMB
	if(hdr->dstptr != 0) {
	  //printf("sending long message directly to %p (%zd bytes)\n", hdr->dstptr, hdr->payload_size);
	  out_long_hdrs.pop();
	  still_more = !(out_short_hdrs.empty() && out_long_hdrs.empty());
#ifdef DETAILED_MESSAGE_TIMING
	  int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
	  unsigned qdepth = out_long_hdrs.size();
	  message_log_state = MSGLOGSTATE_NORMAL;
#endif
          $call hsl_, unlock
#ifdef DETAILED_MESSAGE_TIMING
	  CurrentTime start_time;
#endif
	  send_long(hdr, hdr->dstptr);
#ifdef DETAILED_MESSAGE_TIMING
	  detailed_message_timing.record(timing_idx, peer, hdr->msgid, -1, hdr->num_args*4 + hdr->payload_size, qdepth, start_time, CurrentTime());
#endif
	  delete hdr;
	  count++;
	  continue;
	}

	// are we waiting for the next LMB to become available?
	if(!lmb_w_avail[cur_write_lmb]) {
#ifdef DETAILED_MESSAGE_TIMING
	  // log this if we haven't already
	  int timing_idx = -1;
	  unsigned qdepth = out_long_hdrs.size();
	  if(message_log_state != MSGLOGSTATE_LMBWAIT) {
	    timing_idx = detailed_message_timing.get_next_index();
	    message_log_state = MSGLOGSTATE_LMBWAIT;
	  }
#endif
          $call hsl_, unlock
#ifdef DETAILED_MESSAGE_TIMING
	  CurrentTime now;
	  detailed_message_timing.record(timing_idx, peer, hdr->msgid, -3, hdr->num_args*4 + hdr->payload_size, qdepth, now, now);
#endif
	  break;
	}

	// do we have enough room in the current LMB?
	assert(hdr->payload_size <= lmb_size);
	if((cur_write_offset + hdr->payload_size) <= lmb_size) {
	  // we can send the message - update lmb pointers and remove the
	  //  packet from the queue, and then drop them mutex before
	  //  sending the message
	  char *dest_ptr = lmb_w_bases[cur_write_lmb] + cur_write_offset;
	  cur_write_offset += hdr->payload_size;
          // keep write offset aligned to 128B
          if(cur_write_offset & 0x7f)
            cur_write_offset = ((cur_write_offset >> 7) + 1) << 7;
	  cur_write_count++;
	  out_long_hdrs.pop();
	  still_more = !(out_short_hdrs.empty() && out_long_hdrs.empty());

#ifdef DETAILED_MESSAGE_TIMING
	  int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
	  unsigned qdepth = out_long_hdrs.size();
	  message_log_state = MSGLOGSTATE_NORMAL;
#endif
          $call hsl_, unlock
#ifdef DEBUG_LMB
	  printf("LMB: sending %zd bytes %d->%d, [%p,%p)\n",
		 hdr->payload_size, my_node_id, peer,
		 dest_ptr, dest_ptr + hdr->payload_size);
#endif
#ifdef DETAILED_MESSAGE_TIMING
	  CurrentTime start_time;
#endif
	  send_long(hdr, dest_ptr);
#ifdef DETAILED_MESSAGE_TIMING
	  detailed_message_timing.record(timing_idx, peer, hdr->msgid, cur_write_lmb, hdr->num_args*4 + hdr->payload_size, qdepth, start_time, CurrentTime());
#endif
	  delete hdr;
	  count++;
	  continue;
	} else {
	  // can't send the message, so flip the buffer that's now full
	  int flip_buffer = cur_write_lmb;
	  int flip_count = cur_write_count;
	  lmb_w_avail[cur_write_lmb] = false;
	  cur_write_lmb = (cur_write_lmb + 1) % num_lmbs;
	  cur_write_offset = 0;
	  cur_write_count = 0;

#ifdef DETAILED_MESSAGE_TIMING
	  int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
	  unsigned qdepth = out_long_hdrs.size();
	  message_log_state = MSGLOGSTATE_NORMAL;
#endif
	  // now let go of the lock and send the flip request
          $call hsl_, unlock

#ifdef DEBUG_LMB
	  printf("LMB: flipping buffer %d for %d->%d, [%p,%p), count=%d\n",
		 flip_buffer, my_node_id, peer, lmb_w_bases[flip_buffer],
		 lmb_w_bases[flip_buffer]+lmb_size, flip_count);
#endif
#ifdef ACTIVE_MESSAGE_TRACE
          log_amsg_trace.info("Active Message Request: %d %d 2 0",
			      MSGID_FLIP_REQ, peer);
#endif
#ifdef DETAILED_MESSAGE_TIMING
	  CurrentTime start_time;
#endif
	  CHECK_GASNET( gasnet_AMRequestShort2(peer, MSGID_FLIP_REQ,
                                               flip_buffer, flip_count) );
#ifdef DETAILED_MESSAGE_TIMING
	  detailed_message_timing.record(timing_idx, peer, MSGID_FLIP_REQ, flip_buffer, 8, qdepth, start_time, CurrentTime());
#endif
#ifdef TRACE_MESSAGES
          __sync_fetch_and_add(&sent_messages, 1);
#endif

	  continue;
	}
      }

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
    if(!in_order && (hdr->payload_size <= gasnet_AMMaxMedium()))
      out_short_hdrs.push(hdr);
    else
      out_long_hdrs.push(hdr);
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
    case 1:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium1(peer, hdr->msgid, hdr->payload, 
                                              hdr->payload_size, hdr->args[0]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort1(peer, hdr->msgid, hdr->args[0]) );
      }
      break;

    case 2:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium2(peer, hdr->msgid, hdr->payload, hdr->payload_size,
                                              hdr->args[0], hdr->args[1]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort2(peer, hdr->msgid, hdr->args[0], hdr->args[1]) );
      }
      break;

    case 3:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium3(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				hdr->args[0], hdr->args[1], hdr->args[2]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort3(peer, hdr->msgid,
			       hdr->args[0], hdr->args[1], hdr->args[2]) );
      }
      break;

    case 4:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium4(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort4(peer, hdr->msgid,
			       hdr->args[0], hdr->args[1], hdr->args[2],
			       hdr->args[3]) );
      }
      break;

    case 5:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium5(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort5(peer, hdr->msgid,
			       hdr->args[0], hdr->args[1], hdr->args[2],
			       hdr->args[3], hdr->args[4]) );
      }
      break;

    case 6:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium6(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort6(peer, hdr->msgid,
			       hdr->args[0], hdr->args[1], hdr->args[2],
			       hdr->args[3], hdr->args[4], hdr->args[5]) );
      }
      break;

    case 8:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium8(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5],
				hdr->args[6], hdr->args[7]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort8(peer, hdr->msgid,
			       hdr->args[0], hdr->args[1], hdr->args[2],
			       hdr->args[3], hdr->args[4], hdr->args[5],
			       hdr->args[6], hdr->args[7]) );
      }
      break;

    case 10:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium10(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				 hdr->args[0], hdr->args[1], hdr->args[2],
				 hdr->args[3], hdr->args[4], hdr->args[5],
				 hdr->args[6], hdr->args[7], hdr->args[8],
				 hdr->args[9]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort10(peer, hdr->msgid,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5],
				hdr->args[6], hdr->args[7], hdr->args[8],
				hdr->args[9]) );
      }
      break;

    case 12:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium12(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				 hdr->args[0], hdr->args[1], hdr->args[2],
				 hdr->args[3], hdr->args[4], hdr->args[5],
				 hdr->args[6], hdr->args[7], hdr->args[8],
				 hdr->args[9], hdr->args[10], hdr->args[11]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort12(peer, hdr->msgid,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5],
				hdr->args[6], hdr->args[7], hdr->args[8],
				hdr->args[9], hdr->args[10], hdr->args[11]) );
      }
      break;

    case 14:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium14(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				 hdr->args[0], hdr->args[1], hdr->args[2],
				 hdr->args[3], hdr->args[4], hdr->args[5],
				 hdr->args[6], hdr->args[7], hdr->args[8],
				 hdr->args[9], hdr->args[10], hdr->args[11],
				 hdr->args[12], hdr->args[13]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort14(peer, hdr->msgid,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5],
				hdr->args[6], hdr->args[7], hdr->args[8],
				hdr->args[9], hdr->args[10], hdr->args[11],
				hdr->args[12], hdr->args[13]) );
      }
      break;

    case 16:
      if(hdr->payload_mode != PAYLOAD_NONE) {
	CHECK_GASNET( gasnet_AMRequestMedium16(peer, hdr->msgid, hdr->payload, hdr->payload_size,
				 hdr->args[0], hdr->args[1], hdr->args[2],
				 hdr->args[3], hdr->args[4], hdr->args[5],
				 hdr->args[6], hdr->args[7], hdr->args[8],
				 hdr->args[9], hdr->args[10], hdr->args[11],
				 hdr->args[12], hdr->args[13], hdr->args[14],
				 hdr->args[15]) );
      } else {
	CHECK_GASNET( gasnet_AMRequestShort16(peer, hdr->msgid,
				hdr->args[0], hdr->args[1], hdr->args[2],
				hdr->args[3], hdr->args[4], hdr->args[5],
				hdr->args[6], hdr->args[7], hdr->args[8],
				hdr->args[9], hdr->args[10], hdr->args[11],
				hdr->args[12], hdr->args[13], hdr->args[14],
				hdr->args[15]) );
      }
      break;

    default:
      fprintf(stderr, "need to support short/medium of size=%d\n", hdr->num_args);
      assert(1==2);
    }
  }
  
  void send_long(OutgoingMessage *hdr, void *dest_ptr)
  {
    Realm::DetailedTimer::ScopedPush sp(TIME_AM);

    const size_t max_long_req = gasnet_AMMaxLongRequest();

    // Get a new message ID for this message
    // We know that all medium and long active messages use the
    // BaseMedium class as their base type for sending so the first
    // two fields hdr->args[0] and hdr->args[1] can be used for
    // storing the message ID and the number of chunks
    int message_id_start;
    if(hdr->args[0] == BaseMedium::MESSAGE_ID_MAGIC) {
      assert(hdr->args[1] == BaseMedium::MESSAGE_CHUNKS_MAGIC);
      message_id_start = 0;
      //printf("CASE 1\n");
    } else {
      assert(hdr->args[2] == BaseMedium::MESSAGE_ID_MAGIC);
      assert(hdr->args[3] == BaseMedium::MESSAGE_CHUNKS_MAGIC);
      message_id_start = 2;
      //printf("CASE 2\n");
    }
    hdr->args[message_id_start] = next_outgoing_message_id++;
    int chunks = (hdr->payload_size + max_long_req - 1) / max_long_req;
    hdr->args[message_id_start + 1] = chunks;
    if(hdr->payload_mode == PAYLOAD_SRCPTR) {
      //srcdatapool->record_srcptr(hdr->payload);
      gasnet_handlerarg_t srcptr_lo = ((uint64_t)(hdr->payload)) & 0x0FFFFFFFFULL;
      gasnet_handlerarg_t srcptr_hi = ((uint64_t)(hdr->payload)) >> 32;
      hdr->args[message_id_start + 2] = srcptr_lo;
      hdr->args[message_id_start + 3] = srcptr_hi;
    } else {
      hdr->args[message_id_start + 2] = 0;
      hdr->args[message_id_start + 3] = 0;
    }
      
    for (int i = (chunks-1); i >= 0; i--)
    {
      // every chunk but the last is the max size - the last one is whatever
      //   is left (which may also be the max size if it divided evenly)
      size_t size = ((i < (chunks - 1)) ?
                       max_long_req :
                       (hdr->payload_size - (chunks - 1) * max_long_req));
#ifdef DEBUG_AMREQUESTS
      printf("%d->%d: LONG %d %d %p %zd %p / %x %x %x %x / %x %x %x %x / %x %x %x %x / %x %x %x %x\n",
	     my_node_id, peer, hdr->num_args, hdr->msgid,
	     ((char*)hdr->payload)+(i*max_long_req), size, 
	     ((char*)dest_ptr)+(i*max_long_req),
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
			  hdr->msgid, peer, hdr->num_args, size); 
#endif
      switch(hdr->num_args) {
      case 1:
        // should never get this case since we
        // should always be sending at least two args
        assert(false);
        //gasnet_AMRequestLongAsync1(peer, hdr->msgid, 
        //                      hdr->payload, msg_size, dest_ptr,
        //                      hdr->args[0]);
        break;

      case 2:
        CHECK_GASNET( gasnet_AMRequestLongAsync2(peer, hdr->msgid, 
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1]) );
        break;

      case 3:
        CHECK_GASNET( gasnet_AMRequestLongAsync3(peer, hdr->msgid, 
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2]) );
        break;

      case 4:
        CHECK_GASNET( gasnet_AMRequestLongAsync4(peer, hdr->msgid, 
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3]) );
        break;
      case 5:
        CHECK_GASNET (gasnet_AMRequestLongAsync5(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size,
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4]) );
        break;
      case 6:
        CHECK_GASNET( gasnet_AMRequestLongAsync6(peer, hdr->msgid, 
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5]) );
        break;
      case 7:
        CHECK_GASNET( gasnet_AMRequestLongAsync7(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6]) );
        break;
      case 8:
        CHECK_GASNET( gasnet_AMRequestLongAsync8(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7]) );
        break;
      case 9:
        CHECK_GASNET( gasnet_AMRequestLongAsync9(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8]) );
        break;
      case 10:
        CHECK_GASNET( gasnet_AMRequestLongAsync10(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9]) );
        break;
      case 11:
        CHECK_GASNET( gasnet_AMRequestLongAsync11(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10]) );
        break;
      case 12:
        CHECK_GASNET( gasnet_AMRequestLongAsync12(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10], hdr->args[11]) );
        break;
      case 13:
        CHECK_GASNET( gasnet_AMRequestLongAsync13(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10], hdr->args[11],
                              hdr->args[12]) );
        break;
      case 14:
        CHECK_GASNET( gasnet_AMRequestLongAsync14(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10], hdr->args[11],
                              hdr->args[12], hdr->args[13]) );
        break;
      case 15:
        CHECK_GASNET( gasnet_AMRequestLongAsync15(peer, hdr->msgid,
                              ((char*)hdr->payload)+(i*max_long_req), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10], hdr->args[11],
                              hdr->args[12], hdr->args[13], hdr->args[14]) );
        break;
      case 16:
        CHECK_GASNET( gasnet_AMRequestLongAsync16(peer, hdr->msgid,
                              ((char*)hdr->payload+(i*max_long_req)), size, 
                              ((char*)dest_ptr)+(i*max_long_req),
                              hdr->args[0], hdr->args[1], hdr->args[2],
                              hdr->args[3], hdr->args[4], hdr->args[5],
                              hdr->args[6], hdr->args[7], hdr->args[8],
                              hdr->args[9], hdr->args[10], hdr->args[11],
                              hdr->args[12], hdr->args[13], hdr->args[14],
                              hdr->args[15]) );
        break;

      default:
        fprintf(stderr, "need to support long of size=%d\n", hdr->num_args);
        assert(3==4);
      }
    }
  } 

  gasnet_node_t peer;
  
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
