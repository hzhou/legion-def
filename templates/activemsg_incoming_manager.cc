class IncomingMessageManager {
public:
  IncomingMessageManager(int _nodes, Realm::CoreReservationSet& crs);
  ~IncomingMessageManager(void);

  void add_incoming_message(int sender, IncomingMessage *msg);

  void start_handler_threads(int count, size_t stack_size);

  void shutdown(void);

  IncomingMessage *get_messages(int &sender, bool wait = true);

  void handler_thread_loop(void);

protected:
  int nodes;
  int shutdown_flag;
  IncomingMessage **heads;
  IncomingMessage ***tails;
  int *todo_list; // list of nodes with non-empty message lists
  int todo_oldest, todo_newest;
  $call hsl_cond_decl
  Realm::CoreReservation *core_rsrv;
  std::vector<Realm::Thread *> handler_threads;
};
