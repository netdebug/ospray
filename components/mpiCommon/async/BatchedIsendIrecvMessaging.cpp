// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h> // for Sleep
#else
#  include <time.h>
#endif
#include <chrono>
#include <atomic>
#include <thread>

#include "BatchedIsendIrecvMessaging.h"
#include "../MPICommon.h"

namespace ospray {
  namespace mpi {
    namespace async {

      const int SEND_WINDOW_SIZE = 48;
      const int RECV_WINDOW_SIZE = 48;
      const int PROC_WINDOW_SIZE = 20;

      BatchedIsendIrecvImpl::Group::Group(MPI_Comm comm,
                                          Consumer *consumer,
                                          int32 tag)
        : async::Group(comm,consumer,tag),
          sendThread(this),
          procThread(this),
          recvThread(this),
          shouldExit(false)
      {
        sendThread.handle = std::thread([this](){this->sendThread.run();});
        procThread.handle = std::thread([this](){this->procThread.run();});
        recvThread.handle = std::thread([this](){this->recvThread.run();});
      }

      //using ActiveAction = std::pair<Action*, MPI_Request>;

      void BatchedIsendIrecvImpl::SendThread::run()
      {
        Group  &g = *group;
        Action *actions[SEND_WINDOW_SIZE];
        MPI_Request request[SEND_WINDOW_SIZE];
        int finishedSends[SEND_WINDOW_SIZE];
        // Number of outstanding sends in the actions list
        int numSends = 0;
        while (1) {
          // Each iteration try to get some new stuff to send if we have room,
          // and repeatedly try if we have no data to send.
          if (numSends < SEND_WINDOW_SIZE) {
            do {
              int numGot = g->sendQueue.getSomeFor(actions + numSends,
                                                   SEND_WINDOW_SIZE - numSends,
                                                   std::chrono::milliseconds(1));
              if (g->shouldExit.load()){
                return;
              }

              // New sends are placed at the end of the buffer
              for (uint32_t i = numSends; i < numSends + numGot; i++) {
                Action *action = actions[i];
                SERIALIZED_MPI_CALL(Isend(action->data,action->size,MPI_BYTE,
                      action->addr.rank,g->tag,g->comm, &request[i]));
              }

              numSends += numGot;
            } while (numSends == 0);
          }

          // TODO: Is it ok to wait even if we're exiting? Maybe we'd just get
          // send failed statuses back?
          int numFinished = 0;
          SERIALIZED_MPI_CALL(Testsome(numSends, request, &numFinished,
                                       finishedSends, MPI_STATUSES_IGNORE));

          if (numFinished > 0) {
            for (int i = 0; i < numFinished; ++i) {
              Action *a = actions[finishedSends[i]];
              free(a->data);
              delete a;

              // Mark the entry as finished so we can partition the arrays
              actions[finishedSends[i]] = nullptr;
              request[finishedSends[i]] = MPI_REQUEST_NULL;

              { std::lock_guard<std::mutex> lock(g->flushMutex);
                g->numMessagesDoneSending++;
                if (g->numMessagesDoneSending == g->numMessagesAskedToSend)
                  g->isFlushedCondition.notify_one();
              }
            }

            // Move finished sends to the end of the respective buffers
            std::stable_partition(actions, actions + SEND_WINDOW_SIZE,
                [](const Action *a) { return a != nullptr; });

            std::stable_partition(request, request + SEND_WINDOW_SIZE,
                [](const MPI_Request &r) { return r != MPI_REQUEST_NULL; });

            numSends -= numFinished;
          }
        }
      }

      void BatchedIsendIrecvImpl::RecvThread::run()
      {
        Group &g = *group;

        MPI_Request request[RECV_WINDOW_SIZE];
        Action *actions[RECV_WINDOW_SIZE];
        MPI_Status status;
        int numRequests = 0;
#ifdef _WIN32
        const DWORD sleep_time = 1; // ms --> much longer than 150us
#else
        const timespec sleep_time = timespec{0, 150000};
#endif

        while (1) {
          numRequests = 0;
          // wait for first message
          {
            int msgAvail = 0;
            while (!msgAvail) {
              SERIALIZED_MPI_CALL(Iprobe(MPI_ANY_SOURCE, g.tag, g.comm,
                                         &msgAvail, &status));

              if (g.shouldExit.load())
                return;

              if (msgAvail)
                break;

#ifdef _WIN32
              Sleep(sleep_time);
#else
              // TODO: Can we do a CMake feature test for this_thread::sleep_for
              //       and conditionally use nanosleep?
              nanosleep(&sleep_time, nullptr);
#endif
            }

            Action *action = new Action;
            action->addr = Address(&g, status.MPI_SOURCE);
            SERIALIZED_MPI_CALL(Get_count(&status, MPI_BYTE, &action->size));

            action->data = malloc(action->size);

            SERIALIZED_MPI_CALL(Irecv(action->data, action->size, MPI_BYTE,
                                      status.MPI_SOURCE, status.MPI_TAG,
                                      g.comm, &request[numRequests]));

            actions[numRequests++] = action;
          }

          // ... and add all the other ones that are outstanding, up
          // to max window size
          while (numRequests < RECV_WINDOW_SIZE) {
            int msgAvail;
            SERIALIZED_MPI_CALL(Iprobe(MPI_ANY_SOURCE, g.tag, g.comm,
                                       &msgAvail, &status));

            if (!msgAvail)
              break;
            
            Action *action = new Action;
            action->addr = Address(&g, status.MPI_SOURCE);

            SERIALIZED_MPI_CALL(Get_count(&status,MPI_BYTE,&action->size));

            action->data = malloc(action->size);

            SERIALIZED_MPI_CALL(Irecv(action->data, action->size, MPI_BYTE,
                                      status.MPI_SOURCE, status.MPI_TAG,
                                      g.comm, &request[numRequests]));

            actions[numRequests++] = action;
          }

          // TODO: Is it ok to wait even if we're exiting? Maybe we'd just get
          //       send failed statuses back?
          // now, have certain number of messages available...
          // TODO: We should be using testsome like the sending code
          SERIALIZED_MPI_CALL(Waitall(numRequests, request,
                                      MPI_STATUSES_IGNORE));

          // OK, all actions are valid now
          g.recvQueue.putSome(&actions[0], numRequests);
        }
      }

      void BatchedIsendIrecvImpl::ProcThread::run()
      {
        Group *g = (Group *)this->group;
        while (1) {
          Action *actions[PROC_WINDOW_SIZE];
          size_t numActions = 0;

          while (numActions == 0) {
            numActions = g->recvQueue.getSomeFor(actions,
                                                 PROC_WINDOW_SIZE,
                                                 std::chrono::milliseconds(1));
            if (g->shouldExit.load()){
              return;
            }
          }

          for (size_t i = 0; i < numActions; i++) {
            Action *action = actions[i];
            g->consumer->process(action->addr, action->data, action->size);
            delete action;
          }
        }
      }

      void BatchedIsendIrecvImpl::Group::shutdown()
      {
        if (logMPI) {
          std::cout << "#osp:mpi:BatchIsendIrecvMessaging:Group shutting down"
                    << std::endl;
        }
        shouldExit.store(true);
        sendThread.handle.join();
        recvThread.handle.join();
        procThread.handle.join();
      }

      void BatchedIsendIrecvImpl::init()
      {
        mpi::world.barrier();

        if (logMPI) {
          printf("#osp:mpi:BatchedIsendIrecvMessaging started up %i/%i\n",
                 mpi::world.rank, mpi::world.size);
          fflush(0);
        }

        mpi::world.barrier();
      }

      void BatchedIsendIrecvImpl::shutdown()
      {
        mpi::world.barrier();

        if (logMPI) {
          printf("#osp:mpi:BatchedIsendIrecvMessaging shutting down %i/%i\n",
                 mpi::world.rank, mpi::world.size);
          fflush(0);
        }

        mpi::world.barrier();

        for (uint32_t i = 0; i < myGroups.size(); i++)
          myGroups[i]->shutdown();

        if (logMPI) {
          printf("#osp:mpi:BatchedIsendIrecvMessaging finalizing %i/%i\n",
                 mpi::world.rank, mpi::world.size);
        }

        SERIALIZED_MPI_CALL(Finalize());
      }

      async::Group *BatchedIsendIrecvImpl::createGroup(MPI_Comm comm,
                                                       Consumer *consumer,
                                                       int32 tag)
      {
        assert(consumer);
        Group *g = new Group(comm,consumer,tag);
        assert(g);
        myGroups.push_back(g);
        return g;
      }

      void BatchedIsendIrecvImpl::send(const Address &dest,
                                       void *msgPtr,
                                       int32 msgSize)
      {
        Action *action = new Action;
        action->addr   = dest;
        action->data   = msgPtr;
        action->size   = msgSize;

        Group *g = (Group *)dest.group;
        { std::lock_guard<std::mutex> lock(g->flushMutex);
          g->numMessagesAskedToSend++;
        }

        g->sendQueue.put(action);
      }

      /*! flushes all outgoing messages. note this currently does NOT
          check if there's message incomgin during the flushign ... */
      void BatchedIsendIrecvImpl::flush()
      {
        for (auto g : myGroups)
          g->flush();
      }

      void BatchedIsendIrecvImpl::Group::flush()
      {
        std::unique_lock<std::mutex> lock(flushMutex);
        isFlushedCondition.wait(lock,[&]{
          return (numMessagesDoneSending == numMessagesAskedToSend);
        });
      }
      
    } // ::ospray::mpi::async
  } // ::ospray::mpi
} // ::ospray
