/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/network/default_multiplexer.hpp"

#include "caf/config.hpp"
#include "caf/maybe.hpp"
#include "caf/exception.hpp"
#include "caf/make_counted.hpp"
#include "caf/scheduler/abstract_coordinator.hpp"

#include "caf/io/broker.hpp"
#include "caf/io/middleman.hpp"

#include "caf/io/network/protocol.hpp"
#include "caf/io/network/interfaces.hpp"

#ifdef CAF_WINDOWS
# include <winsock2.h>
# include <ws2tcpip.h> // socklen_t, etc. (MSVC20xx)
# include <windows.h>
# include <io.h>
#else
# include <errno.h>
# include <netdb.h>
# include <fcntl.h>
# include <sys/types.h>
# include <arpa/inet.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
#endif

using std::string;

namespace {

#if defined(CAF_MACOS) || defined(CAF_IOS)
  constexpr int no_sigpipe_flag = SO_NOSIGPIPE;
#elif defined(CAF_WINDOWS)
  constexpr int no_sigpipe_flag = 0; // does not exist on Windows
#else // BSD, Linux or Android
  constexpr int no_sigpipe_flag = MSG_NOSIGNAL;
#endif

// safe ourselves some typing
constexpr auto ipv4 = caf::io::network::protocol::ipv4;
constexpr auto ipv6 = caf::io::network::protocol::ipv6;

// predicate for `ccall` meaning "expected result of f is 0"
bool cc_zero(int value) {
  return value == 0;
}

// predicate for `ccall` meaning "expected result of f is 1"
bool cc_one(int value) {
  return value == 1;
}

// predicate for `ccall` meaning "expected result of f is not -1"
bool cc_not_minus1(int value) {
  return value != -1;
}

// predicate for `ccall` meaning "expected result of f is a valid socket"
bool cc_valid_socket(caf::io::network::native_socket fd) {
  return fd != caf::io::network::invalid_native_socket;
}

// calls a C function and throws a `network_error` if `p` returns false
template <class Predicate, class F, class... Ts>
auto ccall(Predicate p, const char* errmsg, F f, Ts&&... xs)
-> decltype(f(std::forward<Ts>(xs)...)) {
  using namespace caf::io::network;
  auto result = f(std::forward<Ts>(xs)...);
  if (! p(result)) {
    std::ostringstream oss;
    oss << errmsg << ": " << last_socket_error_as_string()
        << " [errno: " << last_socket_error() << "]";
    throw caf::network_error(oss.str());
  }
  return result;
}

} // namespace <anonymous>

namespace caf {
namespace io {
namespace network {

// helper function
std::string local_addr_of_fd(native_socket fd);
uint16_t local_port_of_fd(native_socket fd);
std::string remote_addr_of_fd(native_socket fd);
uint16_t remote_port_of_fd(native_socket fd);

/******************************************************************************
 *                     platform-dependent implementations                     *
 ******************************************************************************/

#ifndef CAF_WINDOWS

  string last_socket_error_as_string() {
    return strerror(errno);
  }

  void nonblocking(native_socket fd, bool new_value) {
    CAF_LOG_TRACE(CAF_ARG(fd) << CAF_ARG(new_value));
    // read flags for fd
    auto rf = ccall(cc_not_minus1, "cannot read flags", fcntl, fd, F_GETFL, 0);
    // calculate and set new flags
    auto wf = new_value ? (rf | O_NONBLOCK) : (rf & (~(O_NONBLOCK)));
    ccall(cc_not_minus1, "cannot set flags", fcntl, fd, F_SETFL, wf);
  }

  void allow_sigpipe(native_socket fd, bool new_value) {
#   ifndef CAF_LINUX
    int value = new_value ? 0 : 1;
    ccall(cc_zero, "cannot set SO_NOSIGPIPE", setsockopt, fd, SOL_SOCKET,
          SO_NOSIGPIPE, &value, static_cast<unsigned>(sizeof(value)));
#   else
    // SO_NOSIGPIPE does not exist on Linux, suppress unused warnings
    static_cast<void>(fd);
    static_cast<void>(new_value);
#   endif
  }

  std::pair<native_socket, native_socket> create_pipe() {
    int pipefds[2];
    if (pipe(pipefds) != 0) {
      perror("pipe");
      exit(EXIT_FAILURE);
    }
    return {pipefds[0], pipefds[1]};
  }

#else // CAF_WINDOWS

  string last_socket_error_as_string() {
    LPTSTR errorText = NULL;
    auto hresult = last_socket_error();
    FormatMessage( // use system message tables to retrieve error text
      FORMAT_MESSAGE_FROM_SYSTEM
      // allocate buffer on local heap for error text
      | FORMAT_MESSAGE_ALLOCATE_BUFFER
      // Important! will fail otherwise, since we're not
      // (and CANNOT) pass insertion parameters
      | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, // unused with FORMAT_MESSAGE_FROM_SYSTEM
      hresult, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPTSTR) & errorText, // output
      0,                    // minimum size for output buffer
      nullptr);             // arguments - see note
    std::string result;
    if (errorText != nullptr) {
      result = errorText;
      // release memory allocated by FormatMessage()
      LocalFree(errorText);
    }
    return result;
  }

  void nonblocking(native_socket fd, bool new_value) {
    u_long mode = new_value ? 1 : 0;
    ccall(cc_zero, "unable to set FIONBIO", ioctlsocket, fd, FIONBIO, &mode);
  }

  void allow_sigpipe(native_socket, bool) {
    // nop; SIGPIPE does not exist on Windows
  }

  /**************************************************************************\
   * Based on work of others;                                               *
   * original header:                                                       *
   *                                                                        *
   * Copyright 2007, 2010 by Nathan C. Myers <ncm@cantrip.org>              *
   * Redistribution and use in source and binary forms, with or without     *
   * modification, are permitted provided that the following conditions     *
   * are met:                                                               *
   *                                                                        *
   * Redistributions of source code must retain the above copyright notice, *
   * this list of conditions and the following disclaimer.                  *
   *                                                                        *
   * Redistributions in binary form must reproduce the above copyright      *
   * notice, this list of conditions and the following disclaimer in the    *
   * documentation and/or other materials provided with the distribution.   *
   *                                                                        *
   * The name of the author must not be used to endorse or promote products *
   * derived from this software without specific prior written permission.  *
   *                                                                        *
   * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS    *
   * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT      *
   * LIMITED  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR *
   * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT   *
   * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, *
   * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT       *
   * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,  *
   * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY  *
   * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT    *
   * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE  *
   * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.   *
  \**************************************************************************/
  std::pair<native_socket, native_socket> create_pipe() {
    socklen_t addrlen = sizeof(sockaddr_in);
    native_socket socks[2] = {invalid_native_socket, invalid_native_socket};
    auto listener = ccall(cc_valid_socket, "socket() failed", socket, AF_INET,
                          SOCK_STREAM, IPPROTO_TCP);
    union {
      sockaddr_in inaddr;
      sockaddr addr;
    } a;
    memset(&a, 0, sizeof(a));
    a.inaddr.sin_family = AF_INET;
    a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.inaddr.sin_port = 0;
    // makes sure all sockets are closed in case of an error
    auto guard = detail::make_scope_guard([&] {
      auto e = WSAGetLastError();
      closesocket(listener);
      closesocket(socks[0]);
      closesocket(socks[1]);
      WSASetLastError(e);
    });
    // bind listener to a local port
    int reuse = 1;
    ccall(cc_zero, "setsockopt() failed", setsockopt, listener, SOL_SOCKET,
          SO_REUSEADDR, reinterpret_cast<char*>(&reuse),
          int{sizeof(reuse)});
    ccall(cc_zero, "bind() failed", bind, listener,
          &a.addr, int{sizeof(a.inaddr)});
    // read the port in use: win32 getsockname may only set the port number
    // (http://msdn.microsoft.com/library/ms738543.aspx):
    memset(&a, 0, sizeof(a));
    ccall(cc_zero, "getsockname() failed", getsockname,
          listener, &a.addr, &addrlen);
    a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.inaddr.sin_family = AF_INET;
    // set listener to listen mode
    ccall(cc_zero, "listen() failed", listen, listener, 1);
    // create read-only end of the pipe
    DWORD flags = 0;
    auto read_fd = ccall(cc_valid_socket, "WSASocketW() failed", WSASocketW,
                         AF_INET, SOCK_STREAM, 0, nullptr, 0, flags);
    ccall(cc_zero, "connect() failed", connect, read_fd,
          &a.addr, int{sizeof(a.inaddr)});
    // get write-only end of the pipe
    auto write_fd = ccall(cc_valid_socket, "accept() failed",
                          accept, listener, nullptr, nullptr);
    closesocket(listener);
    guard.disable();
    return std::make_pair(read_fd, write_fd);
  }

#endif

/******************************************************************************
 *                             epoll() vs. poll()                             *
 ******************************************************************************/

#ifdef CAF_EPOLL_MULTIPLEXER

  // In this implementation, shadow_ is the number of sockets we have
  // registered to epoll.

  default_multiplexer::default_multiplexer(actor_system* sys)
      : multiplexer(sys),
        epollfd_(invalid_native_socket),
        shadow_(1),
        pipe_reader_(*this) {
    init();
    epollfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd_ == -1) {
      CAF_LOG_ERROR("epoll_create1: " << strerror(errno));
      exit(errno);
    }
    // handle at most 64 events at a time
    pollset_.resize(64);
    pipe_ = create_pipe();
    pipe_reader_.init(pipe_.first);
    epoll_event ee;
    ee.events = input_mask;
    ee.data.ptr = &pipe_reader_;
    if (epoll_ctl(epollfd_, EPOLL_CTL_ADD, pipe_reader_.fd(), &ee) < 0) {
      CAF_LOG_ERROR("epoll_ctl: " << strerror(errno));
      exit(errno);
    }
  }

  void default_multiplexer::run() {
    CAF_LOG_TRACE("epoll()-based multiplexer");
    while (shadow_ > 0) {
      int presult = epoll_wait(epollfd_, pollset_.data(),
                               static_cast<int>(pollset_.size()), -1);
      CAF_LOG_DEBUG("epoll_wait() on "      << CAF_ARG(shadow_)
                    << " sockets reported " << CAF_ARG(presult)
                    << " event(s)");
      if (presult < 0) {
        switch (errno) {
          case EINTR: {
            // a signal was caught
            // just try again
            continue;
          }
          default: {
            perror("epoll_wait() failed");
            CAF_CRITICAL("epoll_wait() failed");
          }
        }
      }
      auto iter = pollset_.begin();
      auto last = iter + presult;
      for (; iter != last; ++iter) {
        auto ptr = reinterpret_cast<event_handler*>(iter->data.ptr);
        auto fd = ptr ? ptr->fd() : pipe_.first;
        handle_socket_event(fd, static_cast<int>(iter->events), ptr);
      }
      for (auto& me : events_) {
        handle(me);
      }
      events_.clear();
    }
  }

  void default_multiplexer::handle(const default_multiplexer::event& e) {
    CAF_LOG_TRACE("e.fd = " << CAF_ARG(e.fd) << ", mask = "
                  << CAF_ARG(e.mask));
    // ptr is only allowed to nullptr if fd is our pipe
    // read handle which is only registered for input
    CAF_ASSERT(e.ptr != nullptr || e.fd == pipe_.first);
    if (e.ptr && e.ptr->eventbf() == e.mask) {
      // nop
      return;
    }
    auto old = e.ptr ? e.ptr->eventbf() : input_mask;
    if (e.ptr){
      e.ptr->eventbf(e.mask);
    }
    epoll_event ee;
    ee.events = static_cast<uint32_t>(e.mask);
    ee.data.ptr = e.ptr;
    int op;
    if (e.mask == 0) {
      CAF_LOG_DEBUG("attempt to remove socket " << CAF_ARG(e.fd)
                    << " from epoll");
      op = EPOLL_CTL_DEL;
      --shadow_;
    } else if (old == 0) {
      CAF_LOG_DEBUG("attempt to add socket " << CAF_ARG(e.fd) << " to epoll");
      op = EPOLL_CTL_ADD;
      ++shadow_;
    } else {
      CAF_LOG_DEBUG("modify epoll event mask for socket " << CAF_ARG(e.fd)
                    << ": " << CAF_ARG(old) << " -> " << CAF_ARG(e.mask));
      op = EPOLL_CTL_MOD;
    }
    if (epoll_ctl(epollfd_, op, e.fd, &ee) < 0) {
      switch (last_socket_error()) {
        // supplied file descriptor is already registered
        case EEXIST:
          CAF_LOG_ERROR("file descriptor registered twice");
          --shadow_;
          break;
        // op was EPOLL_CTL_MOD or EPOLL_CTL_DEL,
        // and fd is not registered with this epoll instance.
        case ENOENT:
          CAF_LOG_ERROR(
            "cannot delete file descriptor "
            "because it isn't registered");
          if (e.mask == 0) {
            ++shadow_;
          }
          break;
        default:
          CAF_LOG_ERROR(strerror(errno));
          perror("epoll_ctl() failed");
          CAF_CRITICAL("epoll_ctl() failed");
      }
    }
    if (e.ptr) {
      auto remove_from_loop_if_needed = [&](int flag, operation flag_op) {
        if ((old & flag) && !(e.mask & flag)) {
          e.ptr->removed_from_loop(flag_op);
        }
      };
      remove_from_loop_if_needed(input_mask, operation::read);
      remove_from_loop_if_needed(output_mask, operation::write);
    }
  }

#else // CAF_EPOLL_MULTIPLEXER

  // Let's be honest: the API of poll() sucks. When dealing with 1000 sockets
  // and the very last socket in your pollset triggers, you have to traverse
  // all elements only to find a single event. Even worse, poll() does
  // not give you a way of storing a user-defined pointer in the pollset.
  // Hence, you need to find a pointer to the actual object managing the
  // socket. When using a map, your already dreadful O(n) turns into
  // a worst case of O(n * log n). To deal with this nonsense, we have two
  // vectors in this implementation: pollset_ and shadow_. The former
  // stores our pollset, the latter stores our pointers. Both vectors
  // are sorted by the file descriptor. This allows us to quickly,
  // i.e., O(1), access the actual object when handling socket events.

  default_multiplexer::default_multiplexer(actor_system* sys)
      : multiplexer(sys),
        epollfd_(-1),
        pipe_reader_(*this) {
    init();
    // initial setup
    pipe_ = create_pipe();
    pipe_reader_.init(pipe_.first);
    pollfd pipefd;
    pipefd.fd = pipe_reader_.fd();
    pipefd.events = input_mask;
    pipefd.revents = 0;
    pollset_.push_back(pipefd);
    shadow_.push_back(&pipe_reader_);
  }

  void default_multiplexer::run() {
    CAF_LOG_TRACE("poll()-based multiplexer; " << CAF_ARG(input_mask)
                  << CAF_ARG(output_mask) << CAF_ARG(error_mask));
    // we store the results of poll() in a separate vector , because
    // altering the pollset while traversing it is not exactly a
    // bright idea ...
    struct fd_event {
      native_socket  fd;      // our file descriptor
      short          mask;    // the event mask returned by poll()
      event_handler* ptr;     // nullptr in case of a pipe event
    };
    std::vector<fd_event> poll_res;
    while (! pollset_.empty()) {
      int presult;
      CAF_LOG_DEBUG(CAF_ARG(pollset_.size()));
#     ifdef CAF_WINDOWS
        presult = ::WSAPoll(pollset_.data(),
                            static_cast<ULONG>(pollset_.size()), -1);
#     else
        presult = ::poll(pollset_.data(),
                         static_cast<nfds_t>(pollset_.size()), -1);
#     endif
      if (presult < 0) {
        switch (last_socket_error()) {
          case EINTR: {
            CAF_LOG_DEBUG("received EINTR, try again");
            // a signal was caught
            // just try again
            break;
          }
          case ENOMEM: {
            CAF_LOG_ERROR("poll() failed for reason ENOMEM");
            // there's not much we can do other than try again
            // in hope someone else releases memory
            break;
          }
          default: {
            perror("poll() failed");
            CAF_CRITICAL("poll() failed");
          }
        }
        continue; // rince and repeat
      }
      // scan pollset for events first, because we might alter pollset_
      // while running callbacks (not a good idea while traversing it)
      CAF_LOG_DEBUG("scan pollset for socket events");
      for (size_t i = 0; i < pollset_.size() && presult > 0; ++i) {
        auto& pfd = pollset_[i];
        if (pfd.revents != 0) {
          CAF_LOG_DEBUG("event on socket:" << CAF_ARG(pfd.fd)
                        << CAF_ARG(pfd.revents));
          poll_res.push_back({pfd.fd, pfd.revents, shadow_[i]});
          pfd.revents = 0;
          --presult; // stop as early as possible
        }
      }
      CAF_LOG_DEBUG(CAF_ARG(poll_res.size()));
      for (auto& e : poll_res) {
        // we try to read/write as much as possible by ignoring
        // error states as long as there are still valid
        // operations possible on the socket
        handle_socket_event(e.fd, e.mask, e.ptr);
      }
      CAF_LOG_DEBUG(CAF_ARG(events_.size()));
      poll_res.clear();
      for (auto& me : events_) {
        handle(me);
      }
      events_.clear();
    }
  }

  void default_multiplexer::handle(const default_multiplexer::event& e) {
    CAF_ASSERT(e.fd != invalid_native_socket);
    CAF_ASSERT(pollset_.size() == shadow_.size());
    CAF_LOG_TRACE(CAF_ARG(e.fd) << CAF_ARG(e.mask));
    auto last = pollset_.end();
    auto i = std::lower_bound(pollset_.begin(), last, e.fd,
                              [](const pollfd& lhs, native_socket rhs) {
                                return lhs.fd < rhs;
                              });
    pollfd new_element;
    new_element.fd = e.fd;
    new_element.events = static_cast<short>(e.mask);
    new_element.revents = 0;
    int old_mask = 0;
    if (e.ptr) {
      old_mask = e.ptr->eventbf();
      e.ptr->eventbf(e.mask);
    }
    // calculate shadow of i
    multiplexer_poll_shadow_data::iterator j;
    if (i == last) {
      j = shadow_.end();
    } else {
      j = shadow_.begin();
      std::advance(j, distance(pollset_.begin(), i));
    }
    // modify vectors
    if (i == last) { // append
      if (e.mask != 0) {
        pollset_.push_back(new_element);
        shadow_.push_back(e.ptr);
      }
    } else if (i->fd == e.fd) { // modify
      if (e.mask == 0) {
        // delete item
        pollset_.erase(i);
        shadow_.erase(j);
      } else {
        // update event mask of existing entry
        CAF_ASSERT(*j == e.ptr);
        i->events = static_cast<short>(e.mask);
      }
      if (e.ptr) {
        auto remove_from_loop_if_needed = [&](int flag, operation flag_op) {
          if ((old_mask & flag) && !(e.mask & flag)) {
            e.ptr->removed_from_loop(flag_op);
          }
        };
        remove_from_loop_if_needed(input_mask, operation::read);
        remove_from_loop_if_needed(output_mask, operation::write);
      }
    } else { // insert at iterator pos
      pollset_.insert(i, new_element);
      shadow_.insert(j, e.ptr);
    }
  }

#endif // CAF_EPOLL_MULTIPLEXER

int add_flag(operation op, int bf) {
  switch (op) {
    case operation::read:
      return bf | input_mask;
    case operation::write:
      return bf | output_mask;
    case operation::propagate_error:
      CAF_LOG_ERROR("unexpected operation");
      break;
  }
  // weird stuff going on
  return 0;
}

int del_flag(operation op, int bf) {
  switch (op) {
    case operation::read:
      return bf & ~input_mask;
    case operation::write:
      return bf & ~output_mask;
    case operation::propagate_error:
      CAF_LOG_ERROR("unexpected operation");
      break;
  }
  // weird stuff going on
  return 0;
}

void default_multiplexer::add(operation op, native_socket fd,
                              event_handler* ptr) {
  CAF_ASSERT(fd != invalid_native_socket);
  // ptr == nullptr is only allowed to store our pipe read handle
  // and the pipe read handle is added in the ctor (not allowed here)
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE(CAF_ARG(op) << CAF_ARG(fd));
  new_event(add_flag, op, fd, ptr);
}

void default_multiplexer::del(operation op, native_socket fd,
                              event_handler* ptr) {
  CAF_ASSERT(fd != invalid_native_socket);
  // ptr == nullptr is only allowed when removing our pipe read handle
  CAF_ASSERT(ptr != nullptr || fd == pipe_.first);
  CAF_LOG_TRACE(CAF_ARG(op)<< CAF_ARG(fd));
  new_event(del_flag, op, fd, ptr);
}

void default_multiplexer::wr_dispatch_request(resumable* ptr) {
  intptr_t ptrval = reinterpret_cast<intptr_t>(ptr);
  // on windows, we actually have sockets, otherwise we have file handles
# ifdef CAF_WINDOWS
    auto res = ::send(pipe_.second, reinterpret_cast<socket_send_ptr>(&ptrval),
                      sizeof(ptrval), no_sigpipe_flag);
# else
    auto res = ::write(pipe_.second, &ptrval, sizeof(ptrval));
# endif
  if (res <= 0) {
    // pipe closed, discard resumable
    intrusive_ptr_release(ptr);
  } else if (static_cast<size_t>(res) < sizeof(ptrval)) {
    // must not happen: wrote invalid pointer to pipe
    std::cerr << "[CAF] Fatal error: wrote invalid data to pipe" << std::endl;
    abort();
  }
}

multiplexer::supervisor_ptr default_multiplexer::make_supervisor() {
  class impl : public multiplexer::supervisor {
 public:
    explicit impl(default_multiplexer* thisptr) : this_(thisptr) {
      // nop
    }
    ~impl() {
      auto ptr = this_;
      ptr->dispatch([=] { ptr->close_pipe(); });
    }
 private:
    default_multiplexer* this_;
  };
  return supervisor_ptr{new impl(this)};
}

void default_multiplexer::close_pipe() {
  CAF_LOG_TRACE("");
  del(operation::read, pipe_.first, nullptr);
}

void default_multiplexer::handle_socket_event(native_socket fd, int mask,
                                              event_handler* ptr) {
  CAF_LOG_TRACE(CAF_ARG(fd) << CAF_ARG(mask));
  CAF_ASSERT(ptr != nullptr);
  bool checkerror = true;
  if (mask & input_mask) {
    checkerror = false;
    // ignore read events if a previous event caused
    // this socket to be shut down for reading
    if (! ptr->read_channel_closed())
      ptr->handle_event(operation::read);
  }
  if (mask & output_mask) {
    checkerror = false;
    ptr->handle_event(operation::write);
  }
  if (checkerror && (mask & error_mask)) {
    CAF_LOG_DEBUG("error occured on socket:"
                  << CAF_ARG(fd) << CAF_ARG(last_socket_error())
                  << CAF_ARG(last_socket_error_as_string()));
    ptr->handle_event(operation::propagate_error);
    del(operation::read, fd, ptr);
    del(operation::write, fd, ptr);
  }
}

void default_multiplexer::init() {
# ifdef CAF_WINDOWS
    WSADATA WinsockData;
    if (WSAStartup(MAKEWORD(2, 2), &WinsockData) != 0) {
        CAF_CRITICAL("WSAStartup failed");
    }
# endif
}

default_multiplexer::~default_multiplexer() {
  if (epollfd_ != invalid_native_socket)
    closesocket(epollfd_);
  // close write handle first
  closesocket(pipe_.second);
  // flush pipe before closing it
  nonblocking(pipe_.first, true);
  auto ptr = pipe_reader_.try_read_next();
  while (ptr) {
    intrusive_ptr_release(ptr);
    ptr = pipe_reader_.try_read_next();
  }
  // do cleanup for pipe reader manually, since WSACleanup needs to happen last
  closesocket(pipe_reader_.fd());
  pipe_reader_.init(invalid_native_socket);
# ifdef CAF_WINDOWS
    WSACleanup();
# endif
}


void default_multiplexer::exec_later(resumable* ptr) {
  CAF_ASSERT(ptr);
  CAF_ASSERT(ptr->as_ref_counted_ptr()->get_reference_count() > 0);
  switch (ptr->subtype()) {
    case resumable::io_actor:
    case resumable::function_object:
      wr_dispatch_request(ptr);
      break;
    default:
     system().scheduler().enqueue(ptr);
  }
}

connection_handle default_multiplexer::add_tcp_scribe(abstract_broker* self,
                                                      native_socket fd) {
  CAF_LOG_TRACE("");
  class impl : public scribe {
  public:
    impl(abstract_broker* ptr, default_multiplexer& ref, native_socket sockfd)
        : scribe(ptr, network::conn_hdl_from_socket(sockfd)),
          launched_(false),
          stream_(ref, sockfd) {
      // nop
    }
    void configure_read(receive_policy::config config) override {
      CAF_LOG_TRACE("");
      stream_.configure_read(config);
      if (! launched_)
        launch();
    }
    void ack_writes(bool enable) override {
      CAF_LOG_TRACE(CAF_ARG(enable));
      stream_.ack_writes(enable);
    }
    std::vector<char>& wr_buf() override {
      return stream_.wr_buf();
    }
    std::vector<char>& rd_buf() override {
      return stream_.rd_buf();
    }
    void stop_reading() override {
      CAF_LOG_TRACE("");
      stream_.stop_reading();
      detach(&stream_.backend(), false);
    }
    void flush() override {
      CAF_LOG_TRACE("");
      stream_.flush(this);
    }
    std::string addr() const override {
      return remote_addr_of_fd(stream_.fd());
    }
    uint16_t port() const override {
      return remote_port_of_fd(stream_.fd());
    }
    void launch() {
      CAF_LOG_TRACE("");
      CAF_ASSERT(! launched_);
      launched_ = true;
      stream_.start(this);
    }
 private:
    bool launched_;
    stream stream_;
  };
  auto ptr = make_counted<impl>(self, *this, fd);
  self->add_scribe(ptr);
  return ptr->hdl();
}

accept_handle default_multiplexer::add_tcp_doorman(abstract_broker* self,
                                                   native_socket fd) {
  CAF_LOG_TRACE(CAF_ARG(sock.fd()));
  CAF_ASSERT(sock.fd() != network::invalid_native_socket);
  class impl : public doorman {
  public:
    impl(abstract_broker* ptr, default_multiplexer& ref, native_socket sockfd)
        : doorman(ptr, network::accept_hdl_from_socket(sockfd)),
          acceptor_(ref, sockfd) {
      // nop
    }
    void new_connection() override {
      CAF_LOG_TRACE("");
      auto& dm = acceptor_.backend();
      msg().handle
        = dm.add_tcp_scribe(parent(), std::move(acceptor_.accepted_socket()));
      invoke_mailbox_element(&acceptor_.backend());
    }
    void stop_reading() override {
      CAF_LOG_TRACE("");
      acceptor_.stop_reading();
      detach(&acceptor_.backend(), false);
    }
    void launch() override {
      CAF_LOG_TRACE("");
      acceptor_.start(this);
    }
    std::string addr() const override {
      return local_addr_of_fd(acceptor_.fd());
    }
    uint16_t port() const override {
      return local_port_of_fd(acceptor_.fd());
    }
 private:
    network::acceptor acceptor_;
  };
  auto ptr = make_counted<impl>(self, *this, fd);
  self->add_doorman(ptr);
  return ptr->hdl();
}

connection_handle default_multiplexer::new_tcp_scribe(const std::string& host,
                                                      uint16_t port) {
  auto fd = new_tcp_connection(host, port);
  return connection_handle::from_int(int64_from_native_socket(fd));
}

void default_multiplexer::assign_tcp_scribe(abstract_broker* self,
                                            connection_handle hdl) {
  CAF_LOG_TRACE(CAF_ARG(self->id()) << CAF_ARG(hdl));
  add_tcp_scribe(self, static_cast<native_socket>(hdl.id()));
}


connection_handle default_multiplexer::add_tcp_scribe(abstract_broker* self,
                                                      const std::string& host,
                                                      uint16_t port) {
  CAF_LOG_TRACE(CAF_ARG(self->id()) << CAF_ARG(host) << CAF_ARG(port));
  return add_tcp_scribe(self, new_tcp_connection(host, port));
}

std::pair<accept_handle, uint16_t>
default_multiplexer::new_tcp_doorman(uint16_t port, const char* in,
                                     bool reuse_addr) {
  auto res = new_tcp_acceptor_impl(port, in, reuse_addr);
  return {accept_handle::from_int(int64_from_native_socket(res.first)),
          res.second};
}

void default_multiplexer::assign_tcp_doorman(abstract_broker* ptr,
                                             accept_handle hdl) {
  add_tcp_doorman(ptr, static_cast<native_socket>(hdl.id()));
}

std::pair<accept_handle, uint16_t>
default_multiplexer::add_tcp_doorman(abstract_broker* self, uint16_t port,
                                     const char* host, bool reuse_addr) {
  auto acceptor = new_tcp_acceptor_impl(port, host, reuse_addr);
  auto bound_port = acceptor.second;
  return {add_tcp_doorman(self, acceptor.first), bound_port};
}

/******************************************************************************
 *               platform-independent implementations (finally)               *
 ******************************************************************************/

void tcp_nodelay(native_socket fd, bool new_value) {
  CAF_LOG_TRACE(CAF_ARG(fd) << CAF_ARG(new_value));
  int flag = new_value ? 1 : 0;
  ccall(cc_zero, "unable to set TCP_NODELAY", setsockopt, fd, IPPROTO_TCP,
        TCP_NODELAY, reinterpret_cast<setsockopt_ptr>(&flag),
        socklen_t{sizeof(flag)});
}

bool is_error(ssize_t res, bool is_nonblock) {
  if (res < 0) {
    auto err = last_socket_error();
    if (! is_nonblock || ! would_block_or_temporarily_unavailable(err)) {
      return true;
    }
    // don't report an error in case of
    // spurious wakeup or something similar
  }
  return false;
}

bool read_some(size_t& result, native_socket fd, void* buf, size_t len) {
  CAF_LOG_TRACE(CAF_ARG(fd) << CAF_ARG(len));
  auto sres = ::recv(fd, reinterpret_cast<socket_recv_ptr>(buf), len, 0);
  CAF_LOG_DEBUG(CAF_ARG(len) << CAF_ARG(fd) << CAF_ARG(sres));
  if (is_error(sres, true) || sres == 0) {
    // recv returns 0  when the peer has performed an orderly shutdown
    return false;
  }
  result = (sres > 0) ? static_cast<size_t>(sres) : 0;
  return true;
}

bool write_some(size_t& result, native_socket fd, const void* buf, size_t len) {
  CAF_LOG_TRACE(CAF_ARG(fd) << CAF_ARG(len));
  auto sres = ::send(fd, reinterpret_cast<socket_send_ptr>(buf),
                     len, no_sigpipe_flag);
  CAF_LOG_DEBUG(CAF_ARG(len) << CAF_ARG(fd) << CAF_ARG(sres));
  if (is_error(sres, true))
    return false;
  result = (sres > 0) ? static_cast<size_t>(sres) : 0;
  return true;
}

bool try_accept(native_socket& result, native_socket fd) {
  CAF_LOG_TRACE(CAF_ARG(fd));
  sockaddr_storage addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t addrlen = sizeof(addr);
  result = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);
  CAF_LOG_DEBUG(CAF_ARG(fd) << CAF_ARG(result));
  if (result == invalid_native_socket) {
    auto err = last_socket_error();
    if (! would_block_or_temporarily_unavailable(err)) {
      return false;
    }
  }
  return true;
}

event_handler::event_handler(default_multiplexer& dm, native_socket sockfd)
    : eventbf_(0),
      fd_(sockfd),
      read_channel_closed_(false),
      backend_(dm) {
  CAF_LOG_TRACE(CAF_ARG(sockfd));
  set_fd_flags();
}

event_handler::~event_handler() {
  if (fd_ != invalid_native_socket)
    closesocket(fd_);
}

void event_handler::close_read_channel() {
  if (fd_ == invalid_native_socket || read_channel_closed_)
    return;
  ::shutdown(fd_, 0); // 0 identifies the read channel on Win & UNIX
  read_channel_closed_ = true;
}

void event_handler::set_fd_flags() {
  if (fd_ == invalid_native_socket)
    return;
  // enable nonblocking IO, disable Nagle's algorithm, and suppress SIGPIPE
  nonblocking(fd_, true);
  tcp_nodelay(fd_, true);
  allow_sigpipe(fd_, false);
}

pipe_reader::pipe_reader(default_multiplexer& dm)
    : event_handler(dm, invalid_native_socket) {
  // nop
}

void pipe_reader::removed_from_loop(operation) {
  // nop
}

resumable* pipe_reader::try_read_next() {
  intptr_t ptrval;
  // on windows, we actually have sockets, otherwise we have file handles
# ifdef CAF_WINDOWS
    auto res = recv(fd(), reinterpret_cast<socket_recv_ptr>(&ptrval),
                    sizeof(ptrval), 0);
# else
    auto res = read(fd(), &ptrval, sizeof(ptrval));
# endif
  if (res != sizeof(ptrval))
    return nullptr;
  return reinterpret_cast<resumable*>(ptrval);
}

void pipe_reader::handle_event(operation op) {
  CAF_LOG_TRACE(CAF_ARG(op));
  switch (op) {
    case operation::read: {
      auto cb = try_read_next();
      switch (cb->resume(&backend(), backend().max_throughput())) {
        case resumable::resume_later:
          backend().exec_later(cb);
          break;
        case resumable::done:
          intrusive_ptr_release(cb);
          break;
        default:
          ; // ignored
      }
      break;
    }
    default:
      // nop (simply ignore errors)
      break;
  }
}

void pipe_reader::init(native_socket fd) {
  fd_ = fd;
}

stream::stream(default_multiplexer& backend_ref, native_socket sockfd)
    : event_handler(backend_ref, sockfd),
      read_threshold_(1),
      collected_(0),
      ack_writes_(false),
      writing_(false),
      written_(0) {
  configure_read(receive_policy::at_most(1024));
}

void stream::start(const manager_ptr& mgr) {
  CAF_ASSERT(mgr != nullptr);
  reader_ = mgr;
  backend().add(operation::read, fd(), this);
  read_loop();
}

void stream::configure_read(receive_policy::config config) {
  rd_flag_ = config.first;
  max_ = config.second;
}

void stream::ack_writes(bool x) {
  ack_writes_ = x;
}

void stream::write(const void* buf, size_t num_bytes) {
  CAF_LOG_TRACE(CAF_ARG(num_bytes));
  auto first = reinterpret_cast<const char*>(buf);
  auto last  = first + num_bytes;
  wr_offline_buf_.insert(wr_offline_buf_.end(), first, last);
}

void stream::flush(const manager_ptr& mgr) {
  CAF_ASSERT(mgr != nullptr);
  CAF_LOG_TRACE(CAF_ARG(wr_offline_buf_.size()));
  if (! wr_offline_buf_.empty() && ! writing_) {
    backend().add(operation::write, fd(), this);
    writer_ = mgr;
    writing_ = true;
    write_loop();
  }
}

void stream::stop_reading() {
  CAF_LOG_TRACE("");
  close_read_channel();
  backend().del(operation::read, fd(), this);
}

void stream::removed_from_loop(operation op) {
  switch (op) {
    case operation::read:  reader_.reset(); break;
    case operation::write: writer_.reset(); break;
    case operation::propagate_error: break;
  }
}

void stream::handle_event(operation op) {
  CAF_LOG_TRACE(CAF_ARG(op));
  switch (op) {
    case operation::read: {
      size_t rb; // read bytes
      if (! read_some(rb, fd(),
                      rd_buf_.data() + collected_,
                      rd_buf_.size() - collected_)) {
        reader_->io_failure(&backend(), operation::read);
        backend().del(operation::read, fd(), this);
      } else if (rb > 0) {
        collected_ += rb;
        if (collected_ >= read_threshold_) {
          reader_->consume(&backend(), rd_buf_.data(), collected_);
          read_loop();
        }
      }
      break;
    }
    case operation::write: {
      size_t wb; // written bytes
      if (! write_some(wb, fd(),
                       wr_buf_.data() + written_,
                       wr_buf_.size() - written_)) {
        writer_->io_failure(&backend(), operation::write);
        backend().del(operation::write, fd(), this);
      } else if (wb > 0) {
        written_ += wb;
        CAF_ASSERT(written_ <= wr_buf_.size());
        auto remaining = wr_buf_.size() - written_;
        if (ack_writes_)
          writer_->data_transferred(&backend(), wb,
                                    remaining + wr_offline_buf_.size());
        // prepare next send (or stop sending)
        if (remaining == 0)
          write_loop();
      }
      break;
    }
    case operation::propagate_error:
      if (reader_)
        reader_->io_failure(&backend(), operation::read);
      if (writer_)
        writer_->io_failure(&backend(), operation::write);
      // backend will delete this handler anyway,
      // no need to call backend().del() here
      break;
  }
}

void stream::read_loop() {
  collected_ = 0;
  switch (rd_flag_) {
    case receive_policy_flag::exactly:
      if (rd_buf_.size() != max_)
        rd_buf_.resize(max_);
      read_threshold_ = max_;
      break;
    case receive_policy_flag::at_most:
      if (rd_buf_.size() != max_)
        rd_buf_.resize(max_);
      read_threshold_ = 1;
      break;
    case receive_policy_flag::at_least: {
      // read up to 10% more, but at least allow 100 bytes more
      auto max_size = max_ + std::max<size_t>(100, max_ / 10);
      if (rd_buf_.size() != max_size)
        rd_buf_.resize(max_size);
      read_threshold_ = max_;
      break;
    }
  }
}

void stream::write_loop() {
  CAF_LOG_TRACE(CAF_ARG(wr_buf_.size()) << CAF_ARG(wr_offline_buf_.size()));
  written_ = 0;
  wr_buf_.clear();
  if (wr_offline_buf_.empty()) {
    writing_ = false;
    backend().del(operation::write, fd(), this);
  } else {
    wr_buf_.swap(wr_offline_buf_);
  }
}

acceptor::acceptor(default_multiplexer& backend_ref, native_socket sockfd)
    : event_handler(backend_ref, sockfd),
      sock_(invalid_native_socket) {
  // nop
}

void acceptor::start(const manager_ptr& mgr) {
  CAF_LOG_TRACE(CAF_ARG(accept_sock_.fd()));
  CAF_ASSERT(mgr != nullptr);
  mgr_ = mgr;
  backend().add(operation::read, fd(), this);
}

void acceptor::stop_reading() {
  CAF_LOG_TRACE(CAF_ARG(accept_sock_.fd()));
  close_read_channel();
  backend().del(operation::read, fd(), this);
}

void acceptor::handle_event(operation op) {
  CAF_LOG_TRACE(CAF_ARG(accept_sock_.fd()) << CAF_ARG(op));
  if (mgr_ && op == operation::read) {
    native_socket sockfd = invalid_native_socket;
    if (try_accept(sockfd, fd())) {
      if (sockfd != invalid_native_socket) {
        sock_ = sockfd;
        mgr_->new_connection();
      }
    }
  }
}

void acceptor::removed_from_loop(operation op) {
  CAF_LOG_TRACE(CAF_ARG(accept_sock_.fd()) << CAF_ARG(op));
  if (op == operation::read)
    mgr_.reset();
}

class socket_guard {
public:
  explicit socket_guard(native_socket fd) : fd_(fd) {
    // nop
  }

  ~socket_guard() {
    close();
  }

  native_socket release() {
    auto fd = fd_;
    fd_ = invalid_native_socket;
    return fd;
  }

  void close() {
    if (fd_ != invalid_native_socket) {
      closesocket(fd_);
      fd_ = invalid_native_socket;
    }
  }

private:
  native_socket fd_;
};

#ifdef CAF_WINDOWS
using sa_family_t = unsigned short;
using in_port_t = unsigned short;
#endif

in_addr& addr_of(sockaddr_in& what) {
  return what.sin_addr;
}

sa_family_t& family_of(sockaddr_in& what) {
  return what.sin_family;
}

in_port_t& port_of(sockaddr_in& what) {
  return what.sin_port;
}

in6_addr& addr_of(sockaddr_in6& what) {
  return what.sin6_addr;
}

sa_family_t& family_of(sockaddr_in6& what) {
  return what.sin6_family;
}

in_port_t& port_of(sockaddr_in6& what) {
  return what.sin6_port;
}

in_port_t& port_of(sockaddr& what) {
  switch (what.sa_family) {
    case AF_INET:
      return port_of(reinterpret_cast<sockaddr_in&>(what));
    case AF_INET6:
      return port_of(reinterpret_cast<sockaddr_in6&>(what));
    default:
      break;
  }
  throw std::invalid_argument("invalid protocol family");
}

template <int Family>
bool ip_connect(native_socket fd, const std::string& host, uint16_t port) {
  CAF_LOG_TRACE("Family =" << (Family == AF_INET ? "AF_INET" : "AF_INET6")
                << CAF_ARG(fd) << CAF_ARG(host));
  static_assert(Family == AF_INET || Family == AF_INET6, "invalid family");
  using sockaddr_type =
    typename std::conditional<
      Family == AF_INET,
      sockaddr_in,
      sockaddr_in6
    >::type;
  sockaddr_type sa;
  memset(&sa, 0, sizeof(sockaddr_type));
  inet_pton(Family, host.c_str(), &addr_of(sa));
  family_of(sa) = Family;
  port_of(sa)   = htons(port);
  return connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0;
}

native_socket new_tcp_connection(const std::string& host, uint16_t port,
                                 maybe<protocol> preferred) {
  CAF_LOG_TRACE(CAF_ARG(host) << CAF_ARG(port) << CAF_ARG(preferred));
  CAF_LOG_INFO("try to connect to:" << CAF_ARG(host) << CAF_ARG(port));
  auto res = interfaces::native_address(host, preferred);
  if (! res) {
    CAF_LOG_INFO("no such host");
    throw network_error("no such host: " + host);
  }
  auto proto = res->second;
  CAF_ASSERT(proto == ipv4 || proto == ipv6);
  auto fd = ccall(cc_valid_socket, "socket creation failed", socket,
                  proto == ipv4 ? AF_INET : AF_INET6, SOCK_STREAM, 0);
  socket_guard sguard(fd);
  if (proto == ipv6) {
    if (ip_connect<AF_INET6>(fd, res->first, port)) {
      CAF_LOG_INFO("successfully connected to host via IPv6");
      return sguard.release();
    }
    sguard.close();
    // IPv4 fallback
    return new_tcp_connection(host, port, ipv4);
  }
  if (! ip_connect<AF_INET>(fd, res->first, port)) {
    CAF_LOG_ERROR("could not connect to:" << CAF_ARG(host) << CAF_ARG(port));
    throw network_error("could not connect to " + host);
  }
  CAF_LOG_INFO("successfully connected to host via IPv4");
  return sguard.release();
}

template <class SockAddrType>
void read_port(native_socket fd, SockAddrType& sa) {
  socklen_t len = sizeof(SockAddrType);
  ccall(cc_zero, "read_port failed", getsockname, fd,
        reinterpret_cast<sockaddr*>(&sa), &len);
}

void set_inaddr_any(native_socket, sockaddr_in& sa) {
  sa.sin_addr.s_addr = INADDR_ANY;
}

void set_inaddr_any(native_socket fd, sockaddr_in6& sa) {
  sa.sin6_addr = in6addr_any;
  // also accept ipv4 requests on this socket
  int off = 0;
  ccall(cc_zero, "unable to unset IPV6_V6ONLY", setsockopt, fd, IPPROTO_IPV6,
        IPV6_V6ONLY, reinterpret_cast<setsockopt_ptr>(&off),
        socklen_t{sizeof(off)});
}

template <int Family>
uint16_t new_ip_acceptor_impl(native_socket fd, uint16_t port,
                              const char* addr) {
  static_assert(Family == AF_INET || Family == AF_INET6, "invalid family");
  CAF_LOG_TRACE(CAF_ARG(port) << ", addr = " << (addr ? addr : "nullptr"));
# ifdef CAF_WINDOWS
    // make sure TCP has been initialized via WSAStartup
    get_multiplexer_singleton();
# endif
  using sockaddr_type =
    typename std::conditional<
      Family == AF_INET,
      sockaddr_in,
      sockaddr_in6
    >::type;
  sockaddr_type sa;
  memset(&sa, 0, sizeof(sockaddr_type));
  family_of(sa) = Family;
  if (! addr) {
    set_inaddr_any(fd, sa);
  } else {
    ccall(cc_one, "invalid IP address", inet_pton, Family, addr, &addr_of(sa));
  }
  port_of(sa) = htons(port);
  ccall(cc_zero, "cannot bind socket", bind, fd,
        reinterpret_cast<sockaddr*>(&sa), socklen_t{sizeof(sa)});
  read_port(fd, sa);
  return ntohs(port_of(sa));
}

std::pair<native_socket, uint16_t>
new_tcp_acceptor_impl(uint16_t port, const char* addr, bool reuse_addr) {
  CAF_LOG_TRACE(CAF_ARG(port) << ", addr = " << (addr ? addr : "nullptr"));
  protocol proto = ipv6;
  if (addr) {
    auto addrs = interfaces::native_address(addr);
    if (! addrs) {
      std::string errmsg = "invalid IP address: ";
      errmsg += addr;
      throw network_error(errmsg);
    }
    proto = addrs->second;
    CAF_ASSERT(proto == ipv4 || proto == ipv6);
  }
  auto fd = ccall(cc_valid_socket, "could not create server socket", socket,
                  proto == ipv4 ? AF_INET : AF_INET6, SOCK_STREAM, 0);
  // sguard closes the socket in case of exception
  socket_guard sguard(fd);
  if (reuse_addr) {
    int on = 1;
    ccall(cc_zero, "unable to set SO_REUSEADDR", setsockopt, fd, SOL_SOCKET,
          SO_REUSEADDR, reinterpret_cast<setsockopt_ptr>(&on),
          socklen_t{sizeof(on)});
  }
  auto p = proto == ipv4 ? new_ip_acceptor_impl<AF_INET>(fd, port, addr)
                         : new_ip_acceptor_impl<AF_INET6>(fd, port, addr);
  ccall(cc_zero, "listen() failed", listen, fd, SOMAXCONN);
  // ok, no exceptions so far
  CAF_LOG_DEBUG(CAF_ARG(fd) << CAF_ARG(p));
  return {sguard.release(), p};
}

std::string local_addr_of_fd(native_socket fd) {
  sockaddr_storage st;
  socklen_t st_len = sizeof(st);
  sockaddr* sa = reinterpret_cast<sockaddr*>(&st);
  ccall(cc_zero, "getsockname() failed",
        getsockname, fd, sa, &st_len);
  char addr[INET6_ADDRSTRLEN] {0};
  switch (sa->sa_family) {
    case AF_INET:
      return inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(sa)->sin_addr,
                       addr, sizeof(addr));
    case AF_INET6:
      return inet_ntop(AF_INET6,
                       &reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr,
                       addr, sizeof(addr));
    default:
      break;
  }
  throw std::invalid_argument("invalid protocol family");
}

uint16_t local_port_of_fd(native_socket fd) {
  sockaddr_storage st;
  socklen_t st_len = sizeof(st);
  ccall(cc_zero, "getsockname() failed", getsockname,
        fd, reinterpret_cast<sockaddr*>(&st), &st_len);
  return ntohs(port_of(reinterpret_cast<sockaddr&>(st)));
}

std::string remote_addr_of_fd(native_socket fd) {
  sockaddr_storage st;
  socklen_t st_len = sizeof(st);
  sockaddr* sa = reinterpret_cast<sockaddr*>(&st);
  ccall(cc_zero, "getpeername() failed",
        getpeername, fd, sa, &st_len);
  char addr[INET6_ADDRSTRLEN] {0};
  switch (sa->sa_family) {
    case AF_INET:
      return inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(sa)->sin_addr,
                       addr, sizeof(addr));
    case AF_INET6:
      return inet_ntop(AF_INET6,
                       &reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr,
                       addr, sizeof(addr));
    default:
      break;
  }
  throw std::invalid_argument("invalid protocol family");
}

uint16_t remote_port_of_fd(native_socket fd) {
  sockaddr_storage st;
  socklen_t st_len = sizeof(st);
  ccall(cc_zero, "getpeername() failed", getpeername,
        fd, reinterpret_cast<sockaddr*>(&st), &st_len);
  return ntohs(port_of(reinterpret_cast<sockaddr&>(st)));
}

} // namespace network
} // namespace io
} // namespace caf
