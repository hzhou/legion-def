OutgoingMessage::OutgoingMessage(unsigned _msgid, unsigned _num_args,
				 const void *_args)
  : msgid(_msgid), num_args(_num_args),
    payload(0), payload_size(0), payload_mode(PAYLOAD_NONE), dstptr(0),
    payload_src(0)
{
  for(unsigned i = 0; i < _num_args; i++)
    args[i] = ((const int *)_args)[i];
}
    
OutgoingMessage::~OutgoingMessage(void)
{
  if((payload_mode == PAYLOAD_COPY) || (payload_mode == PAYLOAD_FREE)) {
    if(payload_size > 0) {
#ifdef DEBUG_MEM_REUSE
      for(size_t i = 0; i < payload_size >> 2; i++)
	((unsigned *)payload)[i] = ((0xdc + my_node_id) << 24) + payload_num;
      //memset(payload, 0xdc+my_node_id, payload_size);
      printf("%d: freeing payload %x = [%p, %p)\n",
	     my_node_id, payload_num, payload, ((char *)payload) + payload_size);
#endif
      $call release_spill_memory
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
    $call alloc_srcptr
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
    $call release_spill_memory
  }
}
