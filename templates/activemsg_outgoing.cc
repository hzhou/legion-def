struct OutgoingMessage {
  OutgoingMessage(unsigned _msgid, unsigned _num_args, const void *_args);
  ~OutgoingMessage(void);

  void set_payload(PayloadSource *_payload, size_t _payload_size,
		   int _payload_mode, void *_dstptr = 0);
  inline void set_payload_empty(void) { payload_mode = PAYLOAD_EMPTY; }
  void reserve_srcdata(void);
#if 0
  void set_payload(void *_payload, size_t _payload_size,
		   int _payload_mode, void *_dstptr = 0);
  void set_payload(void *_payload, size_t _line_size,
		   off_t _line_stride, size_t _line_count,
		   int _payload_mode, void *_dstptr = 0);
  void set_payload(const SpanList& spans, size_t _payload_size,
		   int _payload_mode, void *_dstptr = 0);
#endif

  void assign_srcdata_pointer(void *ptr);

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
