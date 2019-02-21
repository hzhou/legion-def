class AM_Manager {
public:
    AM_Manager(){
        $call hsl_, init
        $call cond_, init
        shutdown_flag = false;
    }
    ~AM_Manager(void){}
    void init_corereservation(Realm::CoreReservationSet& crs){
        core_rsrv = new Realm::CoreReservation("AM workers", crs, Realm::CoreReservationParameters());
    }
    void start_threads(){
        p_thread = Realm::Thread::create_kernel_thread<AM_Manager, &AM_Manager::thread_loop>(this, Realm::ThreadLaunchParameters(), *core_rsrv);
    }
    void thread_loop(void){
        $call mpi_thread_loop
    }
    void stop_threads(){
        shutdown_flag = true;
        p_thread->join();
        delete p_thread;
    }
protected:
    $call hsl_cond_decl
    Realm::CoreReservation *core_rsrv;
    Realm::Thread *p_thread;

    bool shutdown_flag;
};

AM_Manager g_am_manager;

/*---------------------------------------------*/
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

void start_handler_threads(int count, Realm::CoreReservationSet&, size_t)
{
    $call mpi_start_handler_threads
}

void stop_activemsg_threads(void)
{
    $call mpi_stop_activemsg_threads
}

