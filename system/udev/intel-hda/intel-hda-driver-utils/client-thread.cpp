// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <intel-hda-driver-utils/client-thread.h>
#include <intel-hda-driver-utils/debug-logging.h>
#include <intel-hda-driver-utils/driver-channel.h>
#include <magenta/cpp.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/port.h>
#include <stdio.h>

mxtl::Mutex ClientThread::pool_lock_;
mx::port    ClientThread::port_;
uint32_t    ClientThread::active_client_count_ = 0;
uint32_t    ClientThread::active_thread_count_ = 0;
mxtl::SinglyLinkedList<mxtl::unique_ptr<ClientThread>> ClientThread::thread_pool_;

ClientThread::ClientThread(uint32_t id) {
    // TODO(johngro) : add the process ID as part of the thread name
    snprintf(name_buffer_, countof(name_buffer_), "ihda-client-%03u", id);
}

void ClientThread::PrintDebugPrefix() const {
    printf("[Thread %s] ", name_buffer_);
}

mx_status_t ClientThread::AddClientLocked() {
    mx_status_t res = NO_ERROR;

    // If we have never added any clients, we will need to start by creating the
    // central port.
    if (!port_.is_valid()) {
        res = mx::port::create(0u, &port_);
        if (res != NO_ERROR) {
            printf("Failed to create client therad pool port (res %d)!\n", res);
            return res;
        }
    }

    active_client_count_++;

    // Try to have as many threads as we have clients, but limit the maximum
    // number of threads to the number of cores in the system.
    //
    // TODO(johngro) : Should we allow users to have more control over the
    // maximum number of threads in the pool?
    while ((active_thread_count_ < active_client_count_) &&
           (active_thread_count_ < mx_system_get_num_cpus())) {

        AllocChecker ac;
        mxtl::unique_ptr<ClientThread> thread(new (&ac) ClientThread(active_thread_count_));

        if (!ac.check()) {
            printf("Out of memory while trying to grow client thread pool!\n");
            active_client_count_--;
            return ERR_NO_MEMORY;
        }

        int musl_res = thrd_create_with_name(
                &thread->thread_,
                [](void* ctx) -> int { return static_cast<ClientThread*>(ctx)->Main(); },
                thread.get(),
                thread->name_buffer_);

        if (musl_res != thrd_success) {
            printf("Failed to create new client thread (res %d)!\n", musl_res);
            active_client_count_--;
            // TODO(johngro) : translate musl error
            return ERR_INTERNAL;
        }

        active_thread_count_++;
        thread_pool_.push_front(mxtl::move(thread));
    }

    return NO_ERROR;
}

void ClientThread::ShutdownPoolLocked() {
    // Don't actually shut the pool unless the number of active clients has
    // dropped to zero.
    if (active_client_count_ > 0)
        return;

    // Have we already been shut down?
    if (!port_.is_valid()) {
        MX_DEBUG_ASSERT(thread_pool_.is_empty());
        return;
    }

    // Close the port.  This should cause all of the threads currently waiting
    // for work to abort and shut down.
    port_.reset();

    while (!thread_pool_.is_empty()) {
        int musl_ret;
        auto thread = thread_pool_.pop_front();

        // TODO(johngro) : Switch to native magenta threads so we can supply a
        // timeout to the join event.
        thrd_join(thread->thread_, &musl_ret);
    }

    active_thread_count_ = 0;
}

int ClientThread::Main() {
    // TODO(johngro) : bump our thread priority to the proper level.
    while (port_.is_valid()) {
        mx_io_packet_t pkt;
        mx_status_t res;

        // Wait for there to be work to dispatch.  If we encounter an error
        // while waiting, it is time to shut down.
        //
        // TODO(johngro) : consider adding a timeout, JiC
        res = port_.wait(MX_TIME_INFINITE, &pkt, sizeof(pkt));
        if (res != NO_ERROR)
            break;

        if (pkt.hdr.type != MX_PORT_PKT_TYPE_IOSN) {
            LOG("Unexpected packet type (%u) in ClientThread pool!\n", pkt.hdr.type);
            continue;
        }

        // Look of the channel which woke up this thread.  If the channel is no
        // longer in the active set, then it is in the process of being torn
        // down and this message should be ignored.
        auto channel = DriverChannel::GetActiveChannel(pkt.hdr.key);
        if (channel != nullptr) {

            if ((pkt.signals & MX_CHANNEL_PEER_CLOSED) != 0) {
                DEBUG_LOG("Peer closed, deactivating channel %" PRIu64 "\n", pkt.hdr.key);
                channel->Deactivate(true);
            } else {
                mx_status_t res = channel->Process(pkt);
                if (res != NO_ERROR) {
                    DEBUG_LOG("Process error (%d), deactivating channel %" PRIu64 " \n",
                              res, pkt.hdr.key);
                    channel->Deactivate(true);
                }
            }

            channel = nullptr;
        }
    }

    DEBUG_LOG("Client work thread shutting down\n");

    return 0;
}
