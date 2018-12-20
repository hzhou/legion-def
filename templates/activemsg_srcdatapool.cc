class SrcDataPool {
public:
  SrcDataPool(void *base, size_t size);
  ~SrcDataPool(void);

  class Lock {
  public:
    Lock(SrcDataPool& _sdp) : sdp(_sdp) { 
        $call hsl_sdp, lock
    }
    ~Lock(void) { 
        $call hsl_sdp, unlock
    }
  protected:
    SrcDataPool& sdp;
  };

  // allocators must already hold the lock - prove it by passing a reference
  void *alloc_srcptr(size_t size_needed, Lock& held_lock);

  // enqueuing a pending message must also hold the lock
  void add_pending(OutgoingMessage *msg, Lock& held_lock);

  // releasing memory will take the lock itself
  void release_srcptr(void *srcptr);

  // attempt to allocate spill memory (may block, and then return false, if limit is hit)
  bool alloc_spill_memory(size_t size_needed, int msgtype, Lock& held_lock, bool first_try);

  // release spilled memory (usually by moving it into actual srcdatapool)
  void release_spill_memory(size_t size_released, int msgtype, Lock& held_lock);

  void print_spill_data(Realm::Logger::LoggingLevel level = Realm::Logger::LEVEL_WARNING);

  static void release_srcptr_handler(gasnet_token_t token, gasnet_handlerarg_t arg0, gasnet_handlerarg_t arg1);

protected:
  size_t round_up_size(size_t size);

  friend class SrcDataPool::Lock;
  $call hsl_cond_decl
  size_t total_size;
  std::map<char *, size_t> free_list;
  std::queue<OutgoingMessage *> pending_allocations;
  // debug
  std::map<char *, size_t> in_use;
  std::map<void *, ssize_t> alloc_counts;

  size_t current_spill_bytes, peak_spill_bytes, current_spill_threshold;
#define TRACK_PER_MESSAGE_SPILLING
#ifdef TRACK_PER_MESSAGE_SPILLING
  size_t current_permsg_spill_bytes[256], peak_permsg_spill_bytes[256];
  size_t total_permsg_spill_bytes[256];
#endif
  int current_suspended_spillers, total_suspended_spillers;
  double total_suspended_time;
public:
  static size_t max_spill_bytes;
  static size_t print_spill_threshold, print_spill_step;
};

static SrcDataPool *srcdatapool = 0;

size_t SrcDataPool::max_spill_bytes = 0;  // default = no limit
size_t SrcDataPool::print_spill_threshold = 1 << 30;  // default = 1 GB
size_t SrcDataPool::print_spill_step = 1 << 30;       // default = 1 GB

// certain threads are exempt from the max spillage due to deadlock concerns
namespace ThreadLocal {
  __thread bool always_allow_spilling = false;
};

// wrapper so we don't have to expose SrcDataPool implementation
void release_srcptr(void *srcptr)
{
  assert(srcdatapool != 0);
  srcdatapool->release_srcptr(srcptr);
}

SrcDataPool::SrcDataPool(void *base, size_t size)
{
  $call hsl_, init
  $call cond_, init
  free_list[(char *)base] = size;
  total_size = size;

  current_spill_bytes = peak_spill_bytes = 0;
#ifdef TRACK_PER_MESSAGE_SPILLING
  for(int i = 0; i < 256; i++)
    current_permsg_spill_bytes[i] = peak_permsg_spill_bytes[i] = total_permsg_spill_bytes[i] = 0;
#endif
  current_spill_threshold = print_spill_threshold;

  current_suspended_spillers = total_suspended_spillers = 0;
  total_suspended_time = 0;
}

SrcDataPool::~SrcDataPool(void)
{
  size_t total = 0;
  size_t nonzero = 0;
  for(std::map<void *, ssize_t>::const_iterator it = alloc_counts.begin(); it != alloc_counts.end(); it++) {
    total++;
    if(it->second != 0) {
      printf("HELP!  srcptr %p on node %d has final count of %zd\n", it->first, my_node_id, it->second);
      nonzero++;
    }
  }
  printf("SrcDataPool:  node %d: %zd total srcptrs, %zd nonzero\n", my_node_id, total, nonzero);
}

size_t SrcDataPool::round_up_size(size_t size)
{
  const size_t BLOCK_SIZE = 64;
  size_t remainder = size % BLOCK_SIZE;
  if(remainder)
    return size + (BLOCK_SIZE - remainder);
  else
    return size;
}

void *SrcDataPool::alloc_srcptr(size_t size_needed, Lock& held_lock)
{
  // sanity check - if the requested size is larger than will ever fit, fail
  if(size_needed > total_size)
    assert(0);

  // early out - if our pending allocation queue is non-empty, they're
  //  first in line, so fail this allocation
  if(!pending_allocations.empty())
    return 0;

  // round up size to something reasonable
  size_needed = round_up_size(size_needed);

  // walk the free list until we find something big enough
  // only use the a bigger chunk if we absolutely have to 
  // in order to avoid segmentation problems.
  std::map<char *, size_t>::iterator it = free_list.begin();
  char *smallest_upper_bound = 0;
  size_t smallest_upper_size = 0;
  while(it != free_list.end()) {
    if(it->second == size_needed) {
      // exact match
      log_sdp.debug("found %p + %zd - exact", it->first, it->second);

      char *srcptr = it->first;
      free_list.erase(it);
      in_use[srcptr] = size_needed;

      return srcptr;
    }

    if(it->second > size_needed) {
      // match with some left over
      // Check to see if it is smaller
      // than the largest upper bound
      if (smallest_upper_bound == 0) {
        smallest_upper_bound = it->first;
        smallest_upper_size = it->second;
      } else if (it->second < smallest_upper_size) {
        smallest_upper_bound = it->first;
        smallest_upper_size = it->second;
      }
    }

    // not big enough - keep looking
    it++;
  }
  if (smallest_upper_bound != 0) {
    it = free_list.find(smallest_upper_bound);
    char *srcptr = it->first + (it->second - size_needed);
    it->second -= size_needed;
    in_use[srcptr] = size_needed;

    log_sdp.debug("found %p + %zd > %zd", it->first, it->second, size_needed);

    return srcptr;
  }

  // allocation failed - let caller decide what to do (probably add it as a
  //   pending allocation after maybe moving data)
  return 0;
}

void SrcDataPool::add_pending(OutgoingMessage *msg, Lock& held_lock)
{
  // simple - just add to our queue
  log_sdp.debug("pending allocation: %zd for %p", msg->payload_size, msg);

  // sanity check - if the requested size is larger than will ever fit, 
  //  we're just dead
  if(msg->payload_size > total_size) {
    log_sdp.error("allocation of %zd can never be satisfied! (max = %zd)",
		  msg->payload_size, total_size);
    assert(0);
  }

  pending_allocations.push(msg);
}

void SrcDataPool::release_srcptr(void *srcptr)
{
  char *srcptr_c = (char *)srcptr;

  log_sdp.debug("releasing srcptr = %p", srcptr);

  // releasing a srcptr span may result in some pending allocations being
  //   satisfied - keep a list so their actual copies can happen without
  //   holding the SDP lock
  std::vector<std::pair<OutgoingMessage *, void *> > satisfied;
  {
    Lock held_lock(*this);

    // look up the pointer to find its size
    std::map<char *, size_t>::iterator it = in_use.find(srcptr_c);
    assert(it != in_use.end());
    size_t size = it->second;
    in_use.erase(it);  // remove from in use list

    // we'd better not be in the free list ourselves
    assert(free_list.find(srcptr_c) == free_list.end());

    // see if we can absorb any adjacent ranges
    if(!free_list.empty()) {
      std::map<char *, size_t>::iterator above = free_list.lower_bound(srcptr_c);
      
      // look below first
      while(above != free_list.begin()) {
	std::map<char *, size_t>::iterator below = above;  below--;
	
	log_sdp.spew("merge?  %p+%zd %p+%zd NONE", below->first, below->second, srcptr_c, size);

	if((below->first + below->second) != srcptr_c)
	  break;

	srcptr_c = below->first;
	size += below->second;
	free_list.erase(below);
      }

      // now look above
      while(above != free_list.end()) {
	log_sdp.spew("merge?  NONE %p+%zd %p+%zd", srcptr_c, size, above->first, above->second);

	if((srcptr_c + size) != above->first)
	  break;

	size += above->second;
	std::map<char *, size_t>::iterator to_nuke(above++);
	free_list.erase(to_nuke);
      }
    }

    // is this possibly-merged span large enough to satisfy the first pending
    //  allocation (if any)?
    if(!pending_allocations.empty() && 
       (size >= pending_allocations.front()->payload_size)) {
      OutgoingMessage *msg = pending_allocations.front();
      pending_allocations.pop();
      size_t act_size = round_up_size(msg->payload_size);
      in_use[srcptr_c] = act_size;
      satisfied.push_back(std::make_pair(msg, srcptr_c));

      // was anything left?  if so, add it to the list of free spans
      if(size > act_size)
	free_list[srcptr_c + act_size] = size - act_size;

      // now see if we can satisfy any other pending allocations - use the
      //  normal allocator routine here because there might be better choices
      //  to use than the span we just freed (assuming any of it is left)
      while(!pending_allocations.empty()) {
	OutgoingMessage *msg = pending_allocations.front();
	void *ptr = alloc_srcptr(msg->payload_size, held_lock);
	if(!ptr) break;

	satisfied.push_back(std::make_pair(msg, ptr));
	pending_allocations.pop();
      }
    } else {
      // no?  then no other span will either, so just add this to the free list
      //  and return
      free_list[srcptr_c] = size;
    }
  }

  // with the lock released, tell any messages that got srcptr's so they can
  //   do their copies
  if(!satisfied.empty())
    for(std::vector<std::pair<OutgoingMessage *, void *> >::iterator it = satisfied.begin();
	it != satisfied.end();
	it++) {
      log_sdp.debug("satisfying pending allocation: %p for %p",
		    it->second, it->first);
      it->first->assign_srcdata_pointer(it->second);
    }
}

bool SrcDataPool::alloc_spill_memory(size_t size_needed, int msgtype, Lock& held_lock,
				     bool first_try)
{
  size_t new_spill_bytes = current_spill_bytes + size_needed;

  // case 1: it fits, so add to total and see if we need to print stuff
  if((max_spill_bytes == 0) || ThreadLocal::always_allow_spilling ||
     (new_spill_bytes <= max_spill_bytes)) {
    current_spill_bytes = new_spill_bytes;
    if(new_spill_bytes > peak_spill_bytes) {
      peak_spill_bytes = new_spill_bytes;
      if(peak_spill_bytes >= current_spill_threshold) {
	current_spill_threshold += print_spill_step;
	print_spill_data();
      }
    }
#ifdef TRACK_PER_MESSAGE_SPILLING
    size_t new_permsg_spill_bytes = current_permsg_spill_bytes[msgtype] + size_needed;
    current_permsg_spill_bytes[msgtype] = new_permsg_spill_bytes;
    if(new_permsg_spill_bytes > peak_permsg_spill_bytes[msgtype])
      peak_permsg_spill_bytes[msgtype] = new_permsg_spill_bytes;
#endif
    return true;
  }
  
  // case 2: we've hit the max allowable spill amount, so stall until room is available

  // sanity-check: would this ever fit?
  assert(size_needed <= max_spill_bytes);

  log_spill.debug() << "max spill amount reached - suspension required ("
		    << current_spill_bytes << " + " << size_needed << " > " << max_spill_bytes;

  // if this is the first try for this message, increase the total waiter count and complain
  current_suspended_spillers++;
  if(first_try) {
    total_suspended_spillers++;
    if(total_suspended_spillers == 1)
      print_spill_data();
  }

  double t1 = Realm::Clock::current_time();

  // sleep until the message would fit, although we won't try to allocate on this pass
  //  (this allows the caller to try the srcdatapool again first)
  while((current_spill_bytes + size_needed) > max_spill_bytes) {
    $call cond_, wait
    log_spill.debug() << "awake - rechecking: "
		      << current_spill_bytes << " + " << size_needed << " > " << max_spill_bytes << "?";
  }

  current_suspended_spillers--;

  double t2 = Realm::Clock::current_time();
  double delta = t2 - t1;

  log_spill.debug() << "spill suspension complete: " << delta << " seconds";
  total_suspended_time += delta;

  return false; // allocation failed, should now be retried
}

void SrcDataPool::release_spill_memory(size_t size_released, int msgtype, Lock& held_lock)
{
  current_spill_bytes -= size_released;

#ifdef TRACK_PER_MESSAGE_SPILLING
  current_permsg_spill_bytes[msgtype] -= size_released;
#endif

  // if there are any threads blocked on spilling data, wake them
  if(current_suspended_spillers > 0) {
    log_spill.debug() << "waking " << current_suspended_spillers << " suspended spillers";
    $call cond_, broadcast
  }
}

void SrcDataPool::print_spill_data(Realm::Logger::LoggingLevel level)
{
  Realm::LoggerMessage msg = log_spill.newmsg(level);

  msg << "current spill usage = "
      << current_spill_bytes << " bytes, peak = " << peak_spill_bytes;
#ifdef TRACK_PER_MESSAGE_SPILLING
  for(int i = 0; i < 256; i++)
    if(total_permsg_spill_bytes[i] > 0)
      msg << "\n"
	  << "  MSG " << i << ": "
	  << "cur=" << current_permsg_spill_bytes[i]
	  << " peak=" << peak_permsg_spill_bytes[i]
	  << " total=" << total_permsg_spill_bytes[i];
#endif
  if(total_suspended_spillers > 0)
    msg << "\n"
	<< "   suspensions=" << total_suspended_spillers
	<< " avg time=" << (total_suspended_time / total_suspended_spillers);
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
  $call AM_token_to_src
  endpoint_manager->record_message(src, false/*sent reply*/);
#endif
}
