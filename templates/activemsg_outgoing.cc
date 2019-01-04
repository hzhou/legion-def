struct OutgoingMessage {
  OutgoingMessage(unsigned _msgid, unsigned _num_args, const void *_args);
  ~OutgoingMessage(void);

  void set_payload(PayloadSource *_payload, size_t _payload_size, int _payload_mode, void *_dstptr = 0);
  inline void set_payload_empty(void) { payload_mode = PAYLOAD_EMPTY; }

  unsigned msgid;
  unsigned num_args;
  void *payload;
  size_t payload_size;
  int payload_mode;
  void *dstptr;
  PayloadSource *payload_src;
  int args[16];
#ifdef DEBUG_MEM_REUSE
  int payload_num;
#endif
};

OutgoingMessage::OutgoingMessage(unsigned _msgid, unsigned _num_args, const void *_args)
  : msgid(_msgid), num_args(_num_args),
    payload(0), payload_size(0), payload_mode(PAYLOAD_NONE), dstptr(0), payload_src(0)
{
  for(unsigned i = 0; i < _num_args; i++)
    args[i] = ((const int *)_args)[i];
}
    
OutgoingMessage::~OutgoingMessage(void)
{
  if((payload_mode == PAYLOAD_COPY) || (payload_mode == PAYLOAD_FREE)) {
    if(payload_size > 0) {
      {
	// TODO: find way to avoid taking lock here?
	SrcDataPool::Lock held_lock(*srcdatapool);
	srcdatapool->release_spill_memory(payload_size, msgid, held_lock);
      }
      //record_spill_free(msgid, payload_size);
      deferred_free(payload);
    }
  }
  if (payload_src != 0) {
    assert(payload_mode == PAYLOAD_KEEPREG);
    delete payload_src;
    payload_src = 0;
  }
}

void OutgoingMessage::set_payload(PayloadSource *_payload_src,
				  size_t _payload_size, int _payload_mode,
				  void *_dstptr)
{
  // die if a payload has already been attached
  assert(payload_mode == PAYLOAD_NONE);
  // We should never be called if either of these are true
  assert(_payload_mode != PAYLOAD_NONE);
  assert(_payload_size > 0);

  // payload must be non-empty, and fit in the LMB unless we have a dstptr for it
  log_sdp.info("setting payload (%zd, %d)", _payload_size, _payload_mode);
  assert((_dstptr != 0) || (_payload_size <= lmb_size));

  // just copy down everything - we won't attempt to grab a srcptr until
  //  we have reserved our spot in an outgoing queue
  dstptr = _dstptr;
  payload = 0;
  payload_size = _payload_size;
  payload_mode = _payload_mode;
  payload_src = _payload_src;
}

// called once we have reserved a space in a given outgoing queue so that
//  we can't get put behind somebody who failed a srcptr allocation (and might
//  need us to go out the door to remove the blockage)
void OutgoingMessage::reserve_srcdata(void)
{
  // no or empty payload cases are easy
  if((payload_mode == PAYLOAD_NONE) ||
     (payload_mode == PAYLOAD_EMPTY)) return;

  // if the payload is stable and in registered memory AND contiguous, we can
  //  just use it
  if((payload_mode == PAYLOAD_KEEPREG) && payload_src->get_contig_pointer()) {
    payload = payload_src->get_contig_pointer();
    return;
  }

  // do we need to place this data in the srcdata pool?
  // for now, yes, unless we don't have a srcdata pool at all
  bool need_srcdata = (srcdatapool != 0);
  
  if(need_srcdata) {
    // try to get the needed space in the srcdata pool
    assert(srcdatapool);

    void *srcptr = 0;
    {
      // take the SDP lock
      SrcDataPool::Lock held_lock(*srcdatapool);

      srcptr = srcdatapool->alloc_srcptr(payload_size, held_lock);
      log_sdp.info("got %p (%d)", srcptr, payload_mode);
	
      if(srcptr != 0) {
	// if we had reserved spill space, we can give it back now
	if((payload_mode == PAYLOAD_COPY) || (payload_mode == PAYLOAD_FREE)) {
	  log_spill.debug() << "returning " << payload_size << " unneeded bytes of spill";
	  srcdatapool->release_spill_memory(payload_size, msgid, held_lock);
	}

	// allocation succeeded - update state, but do copy below, after
	//  we've released the lock
	payload_mode = PAYLOAD_SRCPTR;
	payload = srcptr;
      } else {
	// if the allocation fails, we have to queue ourselves up

	// if we've been instructed to copy the data, that has to happen now
	// SJT: try to figure out/remember why this has to be done with lock held?
	if(payload_mode == PAYLOAD_COPY) {
	  void *copy_ptr = malloc(payload_size);
	  assert(copy_ptr != 0);
	  payload_src->copy_data(copy_ptr);
	  delete payload_src;
	  payload_src = new ContiguousPayload(copy_ptr, payload_size,
					      PAYLOAD_FREE);
	}

	payload_mode = PAYLOAD_PENDING;
	srcdatapool->add_pending(this, held_lock);
      }
    }

    // do the copy now if the allocation succeeded
    if(srcptr != 0) {
      payload_src->copy_data(srcptr);
      delete payload_src;
      payload_src = 0;
    }
  } else {
    // no srcdatapool needed, but might still have to copy
    if(payload_src->get_contig_pointer() &&
       (payload_mode != PAYLOAD_COPY)) {
      payload = payload_src->get_contig_pointer();
      payload_mode = payload_src->get_payload_mode();
      delete payload_src;
      payload_src = 0;
    } else {
      // make a copy
      payload = malloc(payload_size);
      assert(payload != 0);
      payload_src->copy_data(payload);
      delete payload_src;
      payload_src = 0;
      payload_mode = PAYLOAD_FREE;
    }
  }
}

void OutgoingMessage::assign_srcdata_pointer(void *ptr)
{
  assert(payload_mode == PAYLOAD_PENDING);
  assert(payload_src != 0);
  payload_src->copy_data(ptr);

  bool was_using_spill = (payload_src->get_payload_mode() == PAYLOAD_FREE);

  delete payload_src;
  payload_src = 0;

  payload = ptr;
  payload_mode = PAYLOAD_SRCPTR;

  if(was_using_spill) {
    // TODO: find way to avoid taking lock here?
    SrcDataPool::Lock held_lock(*srcdatapool);
    srcdatapool->release_spill_memory(payload_size, msgid, held_lock);
  }
}

