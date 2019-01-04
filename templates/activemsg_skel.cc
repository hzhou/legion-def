/* Copyright 2018 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "realm/realm_config.h"

#define CHECK_PTHREAD(cmd) do { \
  int ret = (cmd); \
  if(ret != 0) { \
    fprintf(stderr, "PTHREAD: %s = %d (%s)\n", #cmd, ret, strerror(ret)); \
    exit(1); \
  } \
} while(0)

#define CHECK_GASNET(cmd) do { \
  int ret = (cmd); \
  if(ret != GASNET_OK) { \
    fprintf(stderr, "GASNET: %s = %d (%s, %s)\n", #cmd, ret, gasnet_ErrorName(ret), gasnet_ErrorDesc(ret)); \
    exit(1); \
  } \
} while(0)

// include gasnet header files before activemsg.h to make sure we have
//  definitions for gasnet_hsl_t and gasnett_cond_t
#ifdef USE_GASNET
$call activemsg_decl_gasnet
#else
$call activemsg_decl_other
#endif

#include "realm/activemsg.h"
#include "realm/cmdline.h"

#include <queue>
#include <assert.h>
#ifdef REALM_PROFILE_AM_HANDLERS
#include <math.h>
#endif

#ifdef DETAILED_MESSAGE_TIMING
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "realm/threads.h"
#include "realm/timers.h"
#include "realm/logging.h"

#define NO_DEBUG_AMREQUESTS

enum { MSGID_LONG_EXTENSION = 253,
       MSGID_FLIP_REQ = 254,
       MSGID_FLIP_ACK = 255 };

// these are changed during gasnet init (if enabled)
NodeID my_node_id = 0;
NodeID max_node_id = 0;

// most of this file assumes the use of gasnet - stubs for a few entry
//  points are defined at the bottom for the !USE_GASNET case
#ifdef USE_GASNET
$call activemsg_impl_gasnet
#elif defined USE_MPI
#include "activemsg_impl_mpi.cc"
#else // defined USE_GASNET
$call activemsg_impl_other
#endif
