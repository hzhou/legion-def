class PayloadSource {
public:
  PayloadSource(void) { }
  virtual ~PayloadSource(void) { }
public:
  virtual void copy_data(void *dest) = 0;
  virtual void *get_contig_pointer(void) { assert(false); return 0; }
  virtual int get_payload_mode(void) { return PAYLOAD_KEEP; }
};

class ContiguousPayload : public PayloadSource {
public:
  ContiguousPayload(void *_srcptr, size_t _size, int _mode);
  virtual ~ContiguousPayload(void) { }
  virtual void copy_data(void *dest);
  virtual void *get_contig_pointer(void) { return srcptr; }
  virtual int get_payload_mode(void) { return mode; }
protected:
  void *srcptr;
  size_t size;
  int mode;
};

class TwoDPayload : public PayloadSource {
public:
  TwoDPayload(const void *_srcptr, size_t _line_size, size_t _line_count,
	      ptrdiff_t _line_stride, int _mode);
  virtual ~TwoDPayload(void) { }
  virtual void copy_data(void *dest);
protected:
  const void *srcptr;
  size_t line_size, line_count;
  ptrdiff_t line_stride;
  int mode;
};

class SpanPayload : public PayloadSource {
public:
  SpanPayload(const SpanList& _spans, size_t _size, int _mode);
  virtual ~SpanPayload(void) { }
  virtual void copy_data(void *dest);
protected:
  SpanList spans;
  size_t size;
  int mode;
};

ContiguousPayload::ContiguousPayload(void *_srcptr, size_t _size, int _mode)
  : srcptr(_srcptr), size(_size), mode(_mode)
{}

void ContiguousPayload::copy_data(void *dest)
{
  log_sdp.info("contig copy %p <- %p (%zd bytes)", dest, srcptr, size);
  memcpy(dest, srcptr, size);
  if(mode == PAYLOAD_FREE)
    free(srcptr);
}

TwoDPayload::TwoDPayload(const void *_srcptr, size_t _line_size,
			 size_t _line_count,
			 ptrdiff_t _line_stride, int _mode)
  : srcptr(_srcptr), line_size(_line_size), line_count(_line_count),
    line_stride(_line_stride), mode(_mode)
{}

void TwoDPayload::copy_data(void *dest)
{
  char *dst_c = (char *)dest;
  const char *src_c = (const char *)srcptr;

  for(size_t i = 0; i < line_count; i++) {
    memcpy(dst_c, src_c, line_size);
    dst_c += line_size;
    src_c += line_stride;
  }
}

SpanPayload::SpanPayload(const SpanList&_spans, size_t _size, int _mode)
  : spans(_spans), size(_size), mode(_mode)
{}

void SpanPayload::copy_data(void *dest)
{
  char *dst_c = (char *)dest;
  size_t bytes_left = size;
  for(SpanList::const_iterator it = spans.begin(); it != spans.end(); it++) {
    assert(it->second <= (size_t)bytes_left);
    memcpy(dst_c, it->first, it->second);
    dst_c += it->second;
    bytes_left -= it->second;
    assert(bytes_left >= 0);
  }
  assert(bytes_left == 0);
}

