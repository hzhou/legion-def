class EndpointManager {
public:
  EndpointManager(int num_endpoints, Realm::CoreReservationSet& crs)
    : total_endpoints(num_endpoints)
  {
    endpoints = new ActiveMessageEndpoint*[num_endpoints];
    for (int i = 0; i < num_endpoints; i++)
    {
      if (i == my_node_id)
        endpoints[i] = 0;
      else
        endpoints[i] = new ActiveMessageEndpoint(i);
    }

    // keep a todo list of endpoints with non-empty queues
    $call hsl_, init
    $call cond_, init
    todo_list = new int[total_endpoints + 1];  // one extra to distinguish full/empty
    todo_oldest = todo_newest = 0;

#ifdef TRACE_MESSAGES
    char filename[80];
    sprintf(filename, "ams_%d.log", my_node_id);
    msgtrace_file = fopen(filename, "w");
    last_msgtrace_report = (int)(Realm::Clock::current_time()); // just keep the integer seconds
#endif

    // for worker threads
    shutdown_flag = false;
    core_rsrv = new Realm::CoreReservation("EndpointManager workers", crs,
					   Realm::CoreReservationParameters());
  }

  ~EndpointManager(void)
  {
#ifdef TRACE_MESSAGES
    report_activemsg_status(msgtrace_file);
    fclose(f);
#endif

    delete[] todo_list;
  }

public:
  void add_todo_entry(int target)
  {
    //printf("%d: adding target %d to list\n", my_node_id, target);
    $call hsl_, lock
    todo_list[todo_newest] = target;
    todo_newest++;
    if(todo_newest > total_endpoints)
      todo_newest = 0;
    assert(todo_newest != todo_oldest); // should never wrap around
    // wake up any sleepers
    $call cond_, broadcast
    $call hsl_, unlock
  }

  void handle_flip_request(gasnet_node_t src, int flip_buffer, int flip_count)
  {
    bool was_empty = endpoints[src]->handle_flip_request(flip_buffer, flip_count);
    if(was_empty)
      add_todo_entry(src);
  }
  void handle_flip_ack(gasnet_node_t src, int ack_buffer)
  {
    endpoints[src]->handle_flip_ack(ack_buffer);
  }

  // returns whether any more messages remain
  bool push_messages(int max_to_send = 0, bool wait = false)
  {
    while(true) {
      // get the next entry from the todo list, waiting if requested
      if(wait) {
        $call hsl_, lock
	while(todo_oldest == todo_newest) {
	  //printf("outgoing todo list is empty - sleeping\n");
          $call cond_, wait
	}
      } else {
	// try to take our lock so we can pop an endpoint from the todo list
        $call trylock_, return true

	// give up if list is empty too
	if(todo_oldest == todo_newest) {
	  // it would be nice to sanity-check here that all endpoints have 
	  //  empty queues, but there's a race condition here with endpoints
	  //  that have added messages but not been able to put themselves on
	  //  the todo list yet
          $call hsl_, unlock
	  return false;
	}
      }

      // have the lock here, and list is non-empty - pop the front one
      int target = todo_list[todo_oldest];
      todo_oldest++;
      if(todo_oldest > total_endpoints)
	todo_oldest = 0;
      $call hsl_, unlock

      //printf("sending messages to %d\n", target);
      bool still_more = endpoints[target]->push_messages(max_to_send, wait);
      //printf("done sending to %d - still more = %d\n", target, still_more);

      // if we didn't send them all, put this target back on the list
      if(still_more)
	add_todo_entry(target);

      // we get stuck in this loop if a sender is waiting on an LMB flip, so make
      //  sure we do some polling inside the loop
      $call ampoll_
    }
  }
  void enqueue_message(gasnet_node_t target, OutgoingMessage *hdr, bool in_order)
  {
    bool was_empty = endpoints[target]->enqueue_message(hdr, in_order);
    if(was_empty)
      add_todo_entry(target);
  }
  void handle_long_msgptr(gasnet_node_t source, const void *ptr)
  {
    bool was_empty = endpoints[source]->handle_long_msgptr(ptr);
    if(was_empty)
      add_todo_entry(source);
  }
  bool adjust_long_msgsize(gasnet_node_t source, void *&ptr, size_t &buffer_size,
                           int message_id, int chunks)
  {
    return endpoints[source]->adjust_long_msgsize(ptr, buffer_size, message_id, chunks);
  }
  void report_activemsg_status(FILE *f)
  {
#ifdef TRACE_MESSAGES
    int mynode = my_node_id;
    for (int i = 0; i < total_endpoints; i++) {
      if (endpoints[i] == 0) continue;

      ActiveMessageEndpoint *e = endpoints[i];
      fprintf(f, "AMS: %d<->%d: S=%d R=%d\n", 
              mynode, i, e->sent_messages, e->received_messages);
    }
    fflush(f);
#else
    // for each node, report depth of outbound queues and LMB state
    int mynode = my_node_id;
    for(int i = 0; i < total_endpoints; i++) {
      if (endpoints[i] == 0) continue;

      ActiveMessageEndpoint *e = endpoints[i];

      fprintf(f, "AMS: %d->%d: S=%zd L=%zd(%zd) W=%d,%d,%zd,%c,%c R=%d,%d\n",
              mynode, i,
              e->out_short_hdrs.size(),
              e->out_long_hdrs.size(), (e->out_long_hdrs.size() ? 
                                        (e->out_long_hdrs.front())->payload_size : 0),
              e->cur_write_lmb, e->cur_write_count, e->cur_write_offset,
              (e->lmb_w_avail[0] ? 'y' : 'n'), (e->lmb_w_avail[1] ? 'y' : 'n'),
              e->lmb_r_counts[0], e->lmb_r_counts[1]);
    }
    fflush(f);
#endif
  }
  void record_message(gasnet_node_t source, bool sent_reply)
  {
    endpoints[source]->record_message(sent_reply);
  }

  void start_polling_threads(int count);

  void stop_threads(void);

protected:
  // runs in a separeate thread
  void polling_worker_loop(void);

private:
  const int total_endpoints;
  ActiveMessageEndpoint **endpoints;
  $call hsl_cond_decl
  int *todo_list;
  int todo_oldest, todo_newest;
  bool shutdown_flag;
  Realm::CoreReservation *core_rsrv;
  std::vector<Realm::Thread *> polling_threads;
#ifdef TRACE_MESSAGES
  FILE *msgtrace_file;
  int last_msgtrace_report;
#endif
};

void EndpointManager::start_polling_threads(int count)
{
  polling_threads.resize(count);
  for(int i = 0; i < count; i++)
    polling_threads[i] = Realm::Thread::create_kernel_thread<EndpointManager, 
							     &EndpointManager::polling_worker_loop>(this,
												    Realm::ThreadLaunchParameters(),
												    *core_rsrv);
}

void EndpointManager::stop_threads(void)
{
  // none of our threads actually sleep, so we can just set the flag and wait for them to notice
  shutdown_flag = true;
  
  for(std::vector<Realm::Thread *>::iterator it = polling_threads.begin();
      it != polling_threads.end();
      it++) {
    (*it)->join();
    delete (*it);
  }
  polling_threads.clear();

#ifdef CHECK_OUTGOING_MESSAGES
  if(todo_oldest != todo_newest) {
    fprintf(stderr, "HELP!  shutdown occured with messages outstanding on node %d!\n", my_node_id);
    while(todo_oldest != todo_newest) {
      int target = todo_list[todo_oldest];
      fprintf(stderr, "target = %d\n", target);
      while(!endpoints[target]->out_short_hdrs.empty()) {
	OutgoingMessage *m = endpoints[target]->out_short_hdrs.front();
	fprintf(stderr, "  SHORT: %d %d %zd\n", m->msgid, m->num_args, m->payload_size);
	endpoints[target]->out_short_hdrs.pop();
      }
      while(!endpoints[target]->out_long_hdrs.empty()) {
	OutgoingMessage *m = endpoints[target]->out_long_hdrs.front();
	fprintf(stderr, "  LONG: %d %d %zd\n", m->msgid, m->num_args, m->payload_size);
	endpoints[target]->out_long_hdrs.pop();
      }
      todo_oldest++;
      if(todo_oldest > total_endpoints)
	todo_oldest = 0;
    }
    //assert(false);
  }
#endif
}

void EndpointManager::polling_worker_loop(void)
{
  while(true) {
    bool still_more = endpoint_manager->push_messages(max_msgs_to_send);

    // check for shutdown, but only if we've pushed all of our messages
    if(shutdown_flag && !still_more)
      break;

    CHECK_GASNET( gasnet_AMPoll() );

#ifdef TRACE_MESSAGES
    // see if it's time to write out another update
    int now = (int)(Realm::Clock::current_time());
    int old = last_msgtrace_report;
    if(now > (old + 29)) {
      // looks like it's time - use an atomic test to see if we should do it
      if(__sync_bool_compare_and_swap(&last_msgtrace_report, old, now))
	report_activemsg_status(msgtrace_file);
    }
#endif
  }
}

