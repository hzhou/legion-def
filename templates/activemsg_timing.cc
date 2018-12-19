static const size_t DEFAULT_MESSAGE_MAX_COUNT = 4 << 20;  // 4 million messages should be plenty

// some helper state to make sure we don't repeatedly log stall conditions
enum {
  MSGLOGSTATE_NORMAL,
  MSGLOGSTATE_SRCDATAWAIT,
  MSGLOGSTATE_LMBWAIT,
};

// little helper that automatically gets the current time
struct CurrentTime {
public:
  CurrentTime(void)
  {
#define USE_REALM_TIMER
#ifdef USE_REALM_TIMER
    now = Realm::Clock::current_time_in_nanoseconds();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sec = ts.tv_sec;
    nsec = ts.tv_nsec;
#endif
  }

#ifdef USE_REALM_TIMER
  long long now;
#else
  unsigned sec, nsec;
#endif
};

struct MessageTimingData {
public:
  unsigned char msg_id;
  char write_lmb;
  unsigned short target;
  unsigned msg_size;
  long long start;
  unsigned dur_nsec;
  unsigned queue_depth;
};

class DetailedMessageTiming {
public:
  DetailedMessageTiming(void)
  {
    path = getenv("LEGION_MESSAGE_TIMING_PATH");
    message_count = 0;
    if(path) {
      char *e = getenv("LEGION_MESSAGE_TIMING_MAX");
      if(e)
	message_max_count = atoi(e);
      else
	message_max_count = DEFAULT_MESSAGE_MAX_COUNT;

      message_timing = new MessageTimingData[message_max_count];
    } else {
      message_max_count = 0;
      message_timing = 0;
    }
  }

  ~DetailedMessageTiming(void)
  {
    delete[] message_timing;
  }

  int get_next_index(void)
  {
    if(message_max_count)
      return __sync_fetch_and_add(&message_count, 1);
    else
      return -1;
  }

  void record(int index, int peer, unsigned char msg_id, char write_lmb, unsigned msg_size, unsigned queue_depth,
	      const CurrentTime& t_start, const CurrentTime& t_end)
  {
    if((index >= 0) && (index < message_max_count)) {
      MessageTimingData &mtd(message_timing[index]);
      mtd.msg_id = msg_id;
      mtd.write_lmb = write_lmb;
      mtd.target = peer;
      mtd.msg_size = msg_size;
#ifdef USE_REALM_TIMER
      mtd.start = t_start.now;
      unsigned long long delta = t_end.now - t_start.now;
#else
      long long now = (1000000000LL * t_start.sec) + t_start.nsec;
      mtd.start = now;
      unsigned long long delta = (t_end.sec - t_start.sec) * 1000000000ULL + t_end.nsec - t_start.nsec;
#endif
      mtd.dur_nsec = (delta > (unsigned)-1) ? ((unsigned)-1) : delta;
      mtd.queue_depth = queue_depth;
    }
  }

  // dump timing data from all the endpoints to a file
  void dump_detailed_timing_data(void)
  {
    if(!path) return;

    char filename[256];
    strcpy(filename, path);
    int l = strlen(filename);
    if(l && (filename[l-1] != '/'))
      filename[l++] = '/';
    sprintf(filename+l, "msgtiming_%d.dat", my_node_id);
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    int count = message_count;
    if(count > message_max_count)
      count = message_max_count;
    ssize_t to_write = count * sizeof(MessageTimingData);
    if(to_write) {
      ssize_t amt = write(fd, message_timing, to_write);
      assert(amt == to_write);
    }

    close(fd);
  }

protected:
  const char *path;
  int message_count, message_max_count;
  MessageTimingData *message_timing;
};
