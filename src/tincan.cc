/*
 * ipop-tincan
 * Copyright 2013, University of Florida
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <algorithm>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef DROID_BUILD
#include "talk/base/ifaddrs-android.h"
#else
#include <ifaddrs.h>
#endif

#include "talk/base/ssladapter.h"

#include "tincanconnectionmanager.h"
#include "controlleraccess.h"
#include "xmppnetwork.h"

#define SEGMENT_SIZE 3
#define SEGMENT_OFFSET 4
#define CMP_SIZE 7

class SendRunnable : public talk_base::Runnable {
 public:
  SendRunnable(thread_opts_t *opts) : opts_(opts) {}

  virtual void Run(talk_base::Thread *thread) {
    ipop_send_thread(opts_);
  }

 private:
  thread_opts_t *opts_;
};

class RecvRunnable : public talk_base::Runnable {
 public:
  RecvRunnable(thread_opts_t *opts) : opts_(opts) {}

  virtual void Run(talk_base::Thread *thread) {
    ipop_recv_thread(opts_);
  }

 private:
  thread_opts_t *opts_;
};

int get_free_network_ip(char *ip_addr, size_t len) {
  struct ifaddrs* interfaces;
  if (getifaddrs(&interfaces) != 0)  return -1;

  // TODO - we should loop again whenever ip address changes
  char tmp_addr[NI_MAXHOST];
  for (struct ifaddrs* ifa = interfaces; ifa != 0; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr != 0 && ifa->ifa_addr->sa_family == AF_INET) {
      int error = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                              tmp_addr, sizeof(tmp_addr), NULL, 0, 
                              NI_NUMERICHOST);
      if (error == 0) {
        if (strncmp(ip_addr, tmp_addr, CMP_SIZE) == 0) {
          char segment[SEGMENT_SIZE] = { '\0' };
          memcpy(segment, ip_addr + SEGMENT_OFFSET, sizeof(segment) - 1);
          int i = atoi(segment) - 1;
          snprintf(ip_addr + SEGMENT_OFFSET, sizeof(segment), "%d", i);
          ip_addr[CMP_SIZE - 1] = '.';  // snprintf adds extra null
        }
      }
    }
  }
  freeifaddrs(interfaces);
  return 0;
}

// TODO - Implement some kind of verification mechanism
bool SSLVerificationCallback(void* cert) {
  return true;
}

int main(int argc, char **argv) {
  talk_base::InitializeSSL(SSLVerificationCallback);
  talk_base::LogMessage::LogToDebug(talk_base::LS_INFO);
  peerlist_init();
  int translate = 1;

  for (int i = argc - 1; i > 0; i--) {
    if (strncmp(argv[i], "-nt", 3) == 0) {
      translate = 0;
    }
  }

  thread_opts_t opts;
  opts.tap = tap_open("ipop", opts.mac);
  if (opts.tap < 0) return -1;
  opts.translate = translate;

  struct threadqueue send_queue, rcv_queue, controller_queue;
  thread_queue_init(&send_queue);
  thread_queue_init(&rcv_queue);
  thread_queue_init(&controller_queue);

  opts.send_queue = &send_queue;
  opts.rcv_queue = &rcv_queue;

  talk_base::Thread packet_handling_thread, send_thread, recv_thread;
  talk_base::AutoThread link_setup_thread;
  link_setup_thread.WrapCurrent();

  tincan::PeerSignalSender signal_sender;
  tincan::TinCanConnectionManager manager(&signal_sender, &link_setup_thread,
                                         &packet_handling_thread, &send_queue, 
                                         &rcv_queue, &controller_queue);
  tincan::XmppNetwork xmpp(&link_setup_thread);
  xmpp.HandlePeer.connect(&manager,
      &tincan::TinCanConnectionManager::HandlePeer);
  talk_base::BasicPacketSocketFactory packet_factory;
  tincan::ControllerAccess controller(manager, xmpp, &packet_factory,
                                       &controller_queue);
  signal_sender.add_service(0, &controller);
  signal_sender.add_service(1, &xmpp);
  opts.send_signal = &tincan::TinCanConnectionManager::HandleQueueSignal;

  // Checks to see if network is available, changes IP if not
  char ip_addr[NI_MAXHOST] = { '\0' };
  manager.ipv4().copy(ip_addr, sizeof(ip_addr));
  if (get_free_network_ip(ip_addr, sizeof(ip_addr)) == 0) {
    manager.set_ip(ip_addr);
  }

  // Setup/run threads
  SendRunnable send_runnable(&opts);
  RecvRunnable recv_runnable(&opts);

  send_thread.Start(&send_runnable);
  recv_thread.Start(&recv_runnable);
  packet_handling_thread.Start();
  link_setup_thread.Run();
  
  return 0;
}

