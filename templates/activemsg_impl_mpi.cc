/*---- polling threads ---------------------*/
class AM_Manager {
public:
    AM_Manager(){
        $call hsl_, init
        $call cond_, init
        shutdown_flag = false;
    }
    ~AM_Manager(void){}
    void init_corereservation(Realm::CoreReservationSet& crs){
        $call _core_rsrv, "AM workers"
    }
    void start_threads(){
        $call _start_polling_threads
    }
    void thread_loop(void){
        $call _polling_thread_loop
    }
    void stop_threads(){
        $call _shutdown_polling_threads
    }
protected:
    $call hsl_cond_decl
    Realm::CoreReservation *core_rsrv;
    Realm::Thread *p_thread;

    std::atomic<bool> shutdown_flag;
};

AM_Manager g_am_manager;

/*---- handler threads ---------------------*/
struct I_Msg {
    I_Msg(int _src, bool _need_free, unsigned int *a, char *b){
        src = _src;
        need_free = _need_free;
        buf_recv = a;
        buf_payload = b;
        next = NULL;
    }

    int src;
    bool need_free;
    unsigned int *buf_recv;
    char *buf_payload;
    struct I_Msg* next;
};

class Incoming_Manager {
public:
    Incoming_Manager(){
        head=NULL;
        num_msgs = 0;
        shutdown_flag = false;
    }
    ~Incoming_Manager(void){}

    void start_threads(Realm::CoreReservationSet& crs, int count, size_t stack_size){
        $call _start_handler_threads
    }
    void thread_loop(void){
        $call _handler_thread_loop
    }
    void shutdown(){
        $call _shutdown_handler_threads
    }
    void enqueue_incoming(int src, bool need_free, unsigned int *buf_recv, char *buf_payload){
        $call _enqueue_incoming
    }
protected:
    $call hsl_cond_decl
    Realm::CoreReservation *core_rsrv;
    bool shutdown_flag;

    struct I_Msg *head, *tail;
    int num_msgs;
    std::vector<Realm::Thread *> handler_threads;
};

Incoming_Manager g_incoming_manager;

/*---- interface functions ----------------*/
void enqueue_message(NodeID target, int msgid,
                    const void *args, size_t arg_size,
                    const void *payload, size_t payload_size,
                    int payload_mode, void *dstptr)
{
    $call mpi_enqueue_message, ContiguousPayload
}

void enqueue_message(NodeID target, int msgid,
                    const void *args, size_t arg_size,
                    const void *payload, size_t line_size,
                    off_t line_stride, size_t line_count,
                    int payload_mode, void *dstptr)
{
    $call mpi_enqueue_message, TwoDPayload
}

void enqueue_message(NodeID target, int msgid,
                    const void *args, size_t arg_size,
                    const SpanList& spans, size_t payload_size,
                    int payload_mode, void *dstptr)
{
    $call mpi_enqueue_message, SpanPayload
}

void do_some_polling(void)
{
    $call mpi_do_some_polling
}

size_t get_lmb_size(NodeID target_node)
{
    // lmb: Long Message Buffer - threshold for breaking up long messages
    // MPI is handling long messages, so we are not breaking it up.
    return std::numeric_limits<size_t>::max();
}

void record_message(NodeID source, bool sent_reply)
{
    $call mpi_record_message
}

void send_srcptr_release(token_t token, uint64_t srcptr)
{
    assert(0);
}

NodeID get_message_source(token_t token)
{
    $call mpi_get_message_source
    return 0;
}

void enqueue_incoming(NodeID sender, IncomingMessage *msg)
{
    $call mpi_enqueue_incoming
    // assert(0);
}

bool adjust_long_msgsize(NodeID source, void *&ptr, size_t &buffer_size,
                        int message_id, int chunks)
{
    $call mpi_adjust_long_msgsize
}

void handle_long_msgptr(NodeID source, const void *ptr)
{
    $call mpi_handle_long_msgptr
}

void add_handler_entry(int msgid, void (*fnptr)())
{
    $call mpi_add_handler_entry
}

void init_endpoints(int gasnet_mem_size_in_mb,
                    int registered_mem_size_in_mb,
                    int registered_ib_mem_size_in_mb,
                    Realm::CoreReservationSet& crs,
                    std::vector<std::string>& cmdline)
{
    $call init_endpoints_mpi
}

void start_polling_threads(int count)
{
    $call mpi_start_polling_threads
}

void start_handler_threads(int count, Realm::CoreReservationSet& crs, size_t stack)
{
    $call mpi_start_handler_threads
}

void stop_activemsg_threads(void)
{
    $call mpi_stop_activemsg_threads
}

