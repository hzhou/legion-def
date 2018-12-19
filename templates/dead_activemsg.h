/* template <int MSGID, class ARGTYPE, void (*FNPTR)(ARGTYPE, const void *, size_t)> */
/* class ActiveMessageMedLongNoReply { */
/*  public: */
/*   static void request(gasnet_node_t dest, gasnet_handler_t handler, */
/* 		      ARGTYPE args, const void *data, size_t datalen) */
/*   { */
/*     HandlerArgUnion<ARGTYPE, sizeof(ARGTYPE)/4> u; */
/*     u.typed = args; */
/*     u.raw.request_medium(dest, handler, data, datalen); */
/*   } */

/*   static int add_handler_entries(gasnet_handlerentry_t *entries) */
/*   { */
/*     entries[0].index = MSGID; */
/*     entries[0].fnptr = FNPTR; */
/*     return 1; */
/*   } */
/* }; */


#if 0 // ifdef USE_GASNET

#define CHECK_GASNET(cmd) cmd

typedef unsigned gasnet_node_t;
#define gasnet_mynode() ((gasnet_node_t)0)
#define gasnet_nodes()  ((gasnet_node_t)1)

#include <pthread.h>

#if 0
// gasnet_hsl_t is a struct containing a pthread_mutex_t
typedef struct {
  //struct { pthread_mutex_t lock; } mutex;
  pthread_mutex_t lock;
} gasnet_hsl_t;
#define GASNET_HSL_INITIALIZER  { PTHREAD_MUTEX_INITIALIZER }

inline void gasnet_hsl_init(gasnet_hsl_t *mutex) 
{ pthread_mutex_init(&(mutex->lock), 0); }
inline void gasnet_hsl_destroy(gasnet_hsl_t *mutex)
{ pthread_mutex_destroy(&(mutex->lock)); }
inline void gasnet_hsl_lock(gasnet_hsl_t *mutex) 
{ pthread_mutex_lock(&(mutex->lock)); }
inline void gasnet_hsl_unlock(gasnet_hsl_t *mutex) 
{ pthread_mutex_unlock(&(mutex->lock)); }
#endif

#define GASNET_WAIT_BLOCK 0
inline void gasnet_set_waitmode(int) {}

// gasnet_hsl_t in object form for templating goodness
class GASNetHSL {
public:
  GASNetHSL(void) { pthread_mutex_init(&mutex, 0); }
  ~GASNetHSL(void) { pthread_mutex_destroy(&mutex); }

private:
  // Should never be copied
  GASNetHSL(const GASNetHSL &rhs) { assert(false); }
  GASNetHSL& operator=(const GASNetHSL &rhs) { assert(false); return *this; }

public:
  void lock(void) { pthread_mutex_lock(&mutex); }
  void unlock(void) { pthread_mutex_unlock(&mutex); }

protected:
  friend class GASNetCondVar;
  pthread_mutex_t mutex;
};

class GASNetCondVar {
public:
  GASNetCondVar(GASNetHSL &_mutex) 
    : mutex(_mutex)
  {
    pthread_cond_init(&cond, 0);
  }

  ~GASNetCondVar(void)
  {
    pthread_cond_destroy(&cond);
  }

  // these require that you hold the lock when you call
  void signal(void)
  {
    pthread_cond_signal(&cond);
  }

  void broadcast(void)
  {
    pthread_cond_broadcast(&cond);
  }

  void wait(void)
  {
    pthread_cond_wait(&cond, &mutex.mutex);
  }

  GASNetHSL &mutex;

protected:
  pthread_cond_t cond;
};

 // barriers
#define GASNET_BARRIERFLAG_ANONYMOUS 0
inline void gasnet_barrier_notify(int, int) {}
inline void gasnet_barrier_wait(int, int) {}

// active message placeholders

typedef int gasnet_handlerentry_t;
typedef int gasnet_handle_t;
typedef struct {
  void *addr;
  size_t size;
} gasnet_seginfo_t;

// define these somewhere so you only get one copy...
extern void *fake_gasnet_mem_base;
extern size_t fake_gasnet_mem_size;

inline void gasnet_init(int*, char ***) {}
inline void gasnet_getSegmentInfo(gasnet_seginfo_t *seginfos, gasnet_node_t count)
{
  seginfos[0].addr = fake_gasnet_mem_base;
  seginfos[0].size = fake_gasnet_mem_size;
}

inline void gasnet_get(void *, int, void *, size_t) { assert(0 && "No GASNet support"); }
inline void gasnet_get_nbi(void *, int, void *, size_t) { assert(0 && "No GASNet support"); }
inline void gasnet_put(int, void *, void *, size_t) { assert(0 && "No GASNet support"); }
inline void gasnet_put_nbi(int, void *, void *, size_t) { assert(0 && "No GASNet support"); }
inline void gasnet_wait_syncnbi_gets(void) {}
inline void gasnet_wait_syncnb(gasnet_handle_t) {}
inline void gasnet_begin_nbi_accessregion(void) {}
inline gasnet_handle_t gasnet_end_nbi_accessregion(void) { return 0; }
inline void gasnet_exit(int code) { exit(code); }

class BaseMedium { public: void *srcptr; };
class BaseReply {};

class ActiveMessagesNotImplemented {
public:
  static int add_handler_entries(gasnet_handlerentry_t *entries, const char *description)
  {
    // no error here - want to allow startup code to run ok
    return 0;
  }
};

template <int MSGID, class MSGTYPE, void (*FNPTR)(MSGTYPE)>
  class ActiveMessageShortNoReply : public ActiveMessagesNotImplemented {
public:
  static void request(gasnet_node_t dest, MSGTYPE args)
  {
    assert(0 && "compiled without USE_GASNET - active messages not available!");
  }
};

template <int MSGID, class MSGTYPE, void (*FNPTR)(MSGTYPE, const void *, size_t)>
class ActiveMessageMediumNoReply : public ActiveMessagesNotImplemented {
public:
  static void request(gasnet_node_t dest, /*const*/ MSGTYPE &args, 
                      const void *data, size_t datalen,
                      int payload_mode, void *dstptr = 0)
  {
    assert(0 && "compiled without USE_GASNET - active messages not available!");
  }

  static void request(gasnet_node_t dest, /*const*/ MSGTYPE &args, 
                      const void *data, size_t line_len,
		      off_t line_stride, size_t line_count,
		      int payload_mode, void *dstptr = 0)
  {
    assert(0 && "compiled without USE_GASNET - active messages not available!");
  }

  static void request(gasnet_node_t dest, /*const*/ MSGTYPE &args, 
                      const SpanList& spans, size_t datalen,
		      int payload_mode, void *dstptr = 0)
  {
    assert(0 && "compiled without USE_GASNET - active messages not available!");
  }
};

template <class T> struct HandlerReplyFuture {
  HandlerReplyFuture(void) : val() {}
  void wait(void) {}
  void set(T newval) { val = newval; }
  T get(void) const { return val; }
  T val;
};

inline void init_endpoints(gasnet_handlerentry_t *handlers, int hcount,
			   int gasnet_mem_size_in_mb,
			   int registered_mem_size_in_mb,
			   int registered_ib_mem_size_in_mb,
			   Realm::CoreReservationSet& crs,
			   std::vector<std::string>& cmdline)
{
  // just use malloc to obtain "gasnet" and/or "registered" memory
  fake_gasnet_mem_size = (gasnet_mem_size_in_mb + 
			  registered_mem_size_in_mb) << 20;
  fake_gasnet_mem_base = malloc(fake_gasnet_mem_size);
}

inline void start_polling_threads(int) {}
inline void start_handler_threads(int, Realm::CoreReservationSet&, size_t) {}
inline void stop_activemsg_threads(void)
{
  if(fake_gasnet_mem_base)
    free(fake_gasnet_mem_base);
  fake_gasnet_mem_base = 0;
}
    
inline void do_some_polling(void) {}
inline size_t get_lmb_size(int target_node) { return 0; }

#endif // ifdef USE_GASNET
