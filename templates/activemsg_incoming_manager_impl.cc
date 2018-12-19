IncomingMessageManager::IncomingMessageManager(int _nodes, Realm::CoreReservationSet& crs)
  : nodes(_nodes), shutdown_flag(0)
{
  heads = new IncomingMessage *[nodes];
  tails = new IncomingMessage **[nodes];
  for(int i = 0; i < nodes; i++) {
    heads[i] = 0;
    tails[i] = 0;
  }
  todo_list = new int[nodes + 1];  // an extra entry to distinguish full from empty
  todo_oldest = todo_newest = 0;
  $call hsl_, init
  $call cond_, init

  core_rsrv = new Realm::CoreReservation("AM handlers", crs,
					 Realm::CoreReservationParameters());
}

IncomingMessageManager::~IncomingMessageManager(void)
{
  delete[] heads;
  delete[] tails;
  delete[] todo_list;
}

void IncomingMessageManager::add_incoming_message(int sender, IncomingMessage *msg)
{
#ifdef DEBUG_INCOMING
  printf("adding incoming message from %d\n", sender);
#endif
  $call hsl_, lock
  if(heads[sender]) {
    // tack this on to the existing list
    assert(tails[sender]);
    *(tails[sender]) = msg;
    tails[sender] = &(msg->next_msg);
  } else {
    // this starts a list, and the node needs to be added to the todo list
    heads[sender] = msg;
    tails[sender] = &(msg->next_msg);
    todo_list[todo_newest] = sender;
    todo_newest++;
    if(todo_newest > nodes)
      todo_newest = 0;
    assert(todo_newest != todo_oldest);  // should never wrap around
    $call cond_, broadcast
  }
  $call hsl_, unlock
}

void IncomingMessageManager::start_handler_threads(int count, size_t stack_size)
{
  handler_threads.resize(count);

  Realm::ThreadLaunchParameters tlp;
  tlp.set_stack_size(stack_size);

  for(int i = 0; i < count; i++)
    handler_threads[i] = Realm::Thread::create_kernel_thread<IncomingMessageManager, 
							     &IncomingMessageManager::handler_thread_loop>(this,
													   tlp,
													   *core_rsrv);
}

void IncomingMessageManager::shutdown(void)
{
  $call hsl_, lock
  if(!shutdown_flag) {
    shutdown_flag = true;
    // wake up any sleepers
    $call cond_, broadcast
  }
  $call hsl_, unlock

  for(std::vector<Realm::Thread *>::iterator it = handler_threads.begin();
      it != handler_threads.end();
      it++) {
    (*it)->join();
    delete (*it);
  }
  handler_threads.clear();
}

IncomingMessage *IncomingMessageManager::get_messages(int &sender, bool wait)
{
  $call hsl_, lock
  while(todo_oldest == todo_newest) {
    // todo list is empty
    if(shutdown_flag || !wait)
      break;
#ifdef DEBUG_INCOMING
    printf("incoming message list is empty - sleeping\n");
#endif
    $call cond_, wait
  }
  IncomingMessage *retval;
  if(todo_oldest == todo_newest) {
    // still empty
    sender = -1;
    retval = 0;
#ifdef DEBUG_INCOMING
    printf("incoming message list is still empty!\n");
#endif
  } else {
    // pop the oldest entry off the todo list
    sender = todo_list[todo_oldest];
    todo_oldest++;
    if(todo_oldest > nodes)
      todo_oldest = 0;
    retval = heads[sender];
    heads[sender] = 0;
    tails[sender] = 0;
#ifdef DEBUG_INCOMING
    printf("handling incoming messages from %d\n", sender);
#endif
  }
  $call hsl_, unlock
  return retval;
}    

void IncomingMessageManager::handler_thread_loop(void)
{
  // messages enqueued in response to incoming messages can never be stalled
  ThreadLocal::always_allow_spilling = true;

  while (true) {
    int sender = -1;
    IncomingMessage *current_msg = get_messages(sender);
    if(!current_msg) {
#ifdef DEBUG_INCOMING
      printf("received empty list - assuming shutdown!\n");
#endif
      break;
    }
#ifdef DETAILED_MESSAGE_TIMING
    int count = 0;
#endif
    while(current_msg) {
      IncomingMessage *next_msg = current_msg->next_msg;
#ifdef DETAILED_MESSAGE_TIMING
      int timing_idx = detailed_message_timing.get_next_index(); // grab this while we still hold the lock
      CurrentTime start_time;
#endif
      current_msg->run_handler();
#ifdef DETAILED_MESSAGE_TIMING
      detailed_message_timing.record(timing_idx, 
				     current_msg->get_peer(),
				     current_msg->get_msgid(),
				     -18, // 0xee - flagged as an incoming message,
				     current_msg->get_msgsize(),
				     count++, // how many messages we handle in a batch
				     start_time, CurrentTime());
#endif
      delete current_msg;
      current_msg = next_msg;
    }
  }
}
