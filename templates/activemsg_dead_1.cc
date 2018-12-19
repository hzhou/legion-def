#if 0
#ifdef TRACK_ACTIVEMSG_SPILL_ALLOCS
size_t current_total_spill_bytes, peak_total_spill_bytes;
size_t current_spill_threshold;
size_t current_spill_step;
size_t current_spill_bytes[256], peak_spill_bytes[256], total_spill_bytes[256];

void init_spill_tracking(void)
{
  if(getenv("ACTIVEMSG_SPILL_THRESHOLD")) {
    // value is in megabytes
    current_spill_threshold = atoi(getenv("ACTIVEMSG_SPILL_THRESHOLD")) << 20;
  } else
    current_spill_threshold = 1 << 30; // 1GB

  if(getenv("ACTIVEMSG_SPILL_STEP")) {
    // value is in megabytes
    current_spill_step = atoi(getenv("ACTIVEMSG_SPILL_STEP")) << 20;
  } else
    current_spill_step = 1 << 30; // 1GB

  current_total_spill_bytes = 0;
  for(int i = 0; i < 256; i++) {
    current_spill_bytes[i] = 0;
    peak_spill_bytes[i] = 0;
    total_spill_bytes[i] = 0;
  }
}
#endif

void print_spill_data(void)
{
  printf("spill node %d: current spill usage = %zd bytes, peak = %zd\n",
	 my_node_id, current_total_spill_bytes, peak_total_spill_bytes);
  for(int i = 0; i < 256; i++)
    if(total_spill_bytes[i] > 0) {
      printf("spill node %d:  MSG %d: cur=%zd peak=%zd total=%zd\n",
	     my_node_id, i,
	     current_spill_bytes[i],
	     peak_spill_bytes[i],
	     total_spill_bytes[i]);
    }
}

void record_spill_alloc(int msgid, size_t bytes)
{
  size_t newcur = __sync_add_and_fetch(&current_spill_bytes[msgid], bytes);
  while(true) {
    size_t oldpeak = peak_spill_bytes[msgid];
    if(oldpeak >= newcur) break;
    if(__sync_bool_compare_and_swap(&peak_spill_bytes[msgid], oldpeak, newcur))
      break;
  }
  __sync_fetch_and_add(&total_spill_bytes[msgid], bytes);
  size_t newtotal = __sync_add_and_fetch(&current_total_spill_bytes, bytes);
  while(true) {
    size_t oldpeak = peak_total_spill_bytes;
    if(oldpeak >= newtotal) break;
    if(__sync_bool_compare_and_swap(&peak_total_spill_bytes, oldpeak, newtotal)) break;
  }
  if(newtotal > current_spill_threshold) {
    current_spill_threshold += current_spill_step;
    print_spill_data();
  }
}

void record_spill_free(int msgid, size_t bytes)
{
  __sync_fetch_and_sub(&current_total_spill_bytes, bytes);
  __sync_fetch_and_sub(&current_spill_bytes[msgid], bytes);
}
#endif
