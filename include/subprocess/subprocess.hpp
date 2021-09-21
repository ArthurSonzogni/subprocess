/*

subprocess.hpp - A no-nonsense library for writing shell commands in C++.

Licensed under the BSL License <https://opensource.org/licenses/BSL-1.0>.
SPDX-License-Identifier: BSL-1.0
Copyright (c) 2021 Rajat Jain.

The documentation can be found at https://subprocess.thecodepad.com.

Permission is hereby granted, free of charge, to any person or organization obtaining a copy of the software
and accompanying documentation covered by this license (the "Software") to use, reproduce, display,
distribute, execute, and transmit the Software, and to prepare derivative works of the Software, and to permit
third-parties to whom the Software is furnished to do so, all subject to the following:

The copyright notices in the Software and this entire statement, including the above license grant, this
restriction and the following disclaimer, must be included in all copies of the Software, in whole or in part,
and all derivative works of the Software, unless such copies or derivative works are solely in the form of
machine-executable object code generated by a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO
EVENT SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER
LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <list>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <vector>

extern "C"
{
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>
}

namespace subprocess
{

namespace exceptions
{

inline namespace _impl
{
static class : public std::error_category
{
public:
  const char* name() const noexcept override { return "subprocess"; }
  std::string message([[maybe_unused]] int condition /*unused*/) const override { return "subprocess_error"; }
} subprocess_category_impl;
}; // namespace _impl

const std::error_category& subprocess_category() { return subprocess_category_impl; }

/**
 * @brief A catch-all class for all errors thrown by subprocess.
 *
 * All exceptions in the library derive from subprocess_error.
 * An exception of type subprocess_error is never actually thrown.
 *
 */
class subprocess_error : public std::system_error
{
public:
  using system_error::system_error;
  subprocess_error(std::string_view what_arg, int errc,
                   const std::error_category& errcat = subprocess_category())
      : system_error{errc, errcat, what_arg.data()}
  {
  }
  subprocess_error(std::initializer_list<std::string> what_arg, int errc,
                   const std::error_category& errcat = subprocess_category())
      : subprocess_error{[&what_arg]
                         {
                           std::ostringstream ostr;
                           std::copy(what_arg.begin(), what_arg.end(),
                                     std::ostream_iterator<std::string>(ostr, " "));
                           return ostr.str();
                         }(),
                         errc, errcat}
  {
  }
};

/**
 * @brief Thrown when there is an error in the usage of the library's public interface
 *
 * These errors should be infrequent on the user end.
 * They get thrown whenever an erroneous call is made to a function within the
 * library.
 *
 * For example, usage_error is thrown when you try to link an fd that is already linked
 * to another fd.
 *
 */
class usage_error : public subprocess_error
{
public:
  explicit usage_error(std::string_view what_arg) : subprocess_error{what_arg, -1} {}
};

/**
 * @brief Thrown when there is an error at the operating system level
 *
 * Thrown whenever an error code is returned from a syscall and
 * subprocess is unable to proceed with the function called by
 * the user.
 *
 */
class os_error : public subprocess_error
{
public:
  using subprocess_error::subprocess_error;
  os_error(std::string_view what_arg, int errc = errno)
      : subprocess_error{what_arg, errc, std::generic_category()}
  {
  }
  os_error(std::initializer_list<std::string> what_arg, int errc = errno)
      : subprocess_error{std::move(what_arg), errc, std::generic_category()}
  {
  }
};

/**
 * @brief Thrown when a command exits with a non-zero exit code.
 *
 * This exception is thrown whenever a subprocess constructed from subprocess::command
 * exits with an error. The code() member function can be called to get the return code.
 */

class command_error : public subprocess_error
{
public:
  using subprocess_error::subprocess_error;
};
} // namespace exceptions

namespace posix_util
{

enum class standard_filenos
{
  standard_in    = STDIN_FILENO,
  standard_out   = STDOUT_FILENO,
  standard_error = STDERR_FILENO,
  max_standard_fd
};

/**
 * @brief POSIX shell argument expander
 *
 * This class wraps the wordexp syscall in a RAII wrapper. wordexp is a POSIX
 * system call that emulates shell parsing for a string as shell would.
 *
 * The return type of wordexp includes includes a delimited string containing
 * args for the function that needs to be called.
 *
 * @see https://linux.die.net/man/3/wordexp
 *
 */
class shell_expander
{
public:
  explicit shell_expander(const std::string& s) { ::wordexp(s.c_str(), &parsed_args_, 0); }
  shell_expander(const shell_expander&)     = default;
  shell_expander(shell_expander&&) noexcept = default;
  shell_expander& operator=(const shell_expander&) = default;
  shell_expander& operator=(shell_expander&&) noexcept = default;
  ~shell_expander() { ::wordfree(&parsed_args_); }

  [[nodiscard]] decltype(std::declval<wordexp_t>().we_wordv) argv() const& { return parsed_args_.we_wordv; }

private:
  ::wordexp_t parsed_args_{};
};

} // namespace posix_util

/**
 * @brief Abstracts file descriptors
 *
 * Member functions of this class and its descendents
 * wrap syscalls to commonly used file descriptor functions.
 *
 * As a user, you can derive from this class to implement your own
 * custom descriptors.
 *
 */
class descriptor
{
public:
  descriptor() = default;
  explicit descriptor(int fd) : fd_{fd} {}
  descriptor(const descriptor&)     = default;
  descriptor(descriptor&&) noexcept = default;
  descriptor& operator=(const descriptor&) = default;
  descriptor& operator=(descriptor&&) noexcept = default;
  virtual ~descriptor()                        = default;

  /**
   * @brief Returns the encapsulated file descriptor
   *
   * The return value of fd() is used by subprocess::process
   * and sent to child processes. This is the fd() that the
   * process will read/write from.
   *
   * @return int OS-level file descriptor for subprocess I/O
   */
  [[nodiscard]] int fd() const { return fd_; }

  /**
   * @brief Marks whether the subprocess should close the FD
   *
   * @return true The subprocess should close the FD
   * @return false The subprocess shouldn't close the FD
   */
  [[nodiscard]] virtual bool closable() const { return false; }

  /**
   * @brief Initialize call before the process runs
   *
   * open() is called by subprocess::execute() right before
   * spawning the child process. This is the function to write
   * for the set up of your I/O from the process.
   */
  virtual void open() {}

  /**
   * @brief Tear down the descriptor
   *
   * close() is called by subprocess::execute() after the
   * process is spawned, but before waiting. This should
   * ideally be the place where you should tear down the constructs
   * that were required for process I/O.
   */
  virtual void close() {}

protected:
  int fd_{-1};
};

/**
 * @brief A tag representing stdin of a process
 *
 */
struct in_t
{
};

/**
 * @brief A tag representing stdout of a process
 *
 */
struct out_t
{
};

/**
 * @brief A tag representing stderr of a process
 *
 */
struct err_t
{
};

static inline in_t in;
static inline out_t out;
static inline err_t err;

/**
 * @brief A shorthand for describing a descriptor ptr
 *
 * @tparam T descriptor or one of its derived classes
 */
template <typename T>
using descriptor_ptr_t = std::enable_if_t<std::is_base_of_v<descriptor, T>, std::shared_ptr<T>>;

/**
 * @brief A shorthand for denoting an owning-pointer for the descriptor base class
 *
 */
using descriptor_ptr = descriptor_ptr_t<descriptor>;

/**
 * @brief Wraps std::make_unique. Used only for self-documentation purposes.
 *
 * @tparam T The type of descriptor to create.
 * @tparam Args
 * @param args Constructor args that should be passed to the descriptor's constructor
 * @return descriptor_ptr_t<T> Returns a descriptor_ptr
 */
template <typename T, typename... Args> inline descriptor_ptr_t<T> make_descriptor(Args&&... args)
{
  return std::make_shared<T>(std::forward<Args>(args)...);
}

/**
 * @brief Adds write ability to descriptor
 *
 * Additionally, the class is marked closable. All output descriptors
 * inherit from this class.
 */
class odescriptor : public virtual descriptor
{
public:
  using descriptor::descriptor;
  /**
   * @brief Writes a given string to fd
   *
   * @param input
   */
  virtual void write(std::string& input);
  void close() override;
  [[nodiscard]] bool closable() const override { return fd() >= 0; }
};

inline void odescriptor::write(std::string& input)
{
  ssize_t total         = static_cast<ssize_t>(input.size());
  std::ptrdiff_t offset = 0;
  while (total > 0)
  {
    if (ssize_t len; (len = ::write(fd(), input.c_str() + offset, total)) >= 0)
    {
      offset += len;
      total -= len;
    }
    else
    {
      throw exceptions::os_error{"write"};
    }
  }
}

inline void odescriptor::close()
{
  if (closable())
  {
    ::close(fd());
    fd_ = -1;
  }
}

/**
 * @brief Adds read ability to descriptor
 *
 * Additionally, the class is marked closable. All input descriptors
 * inherit from this class.
 */
class idescriptor : public virtual descriptor
{
public:
  using descriptor::descriptor;
  /**
   * @brief Read from fd and return std::string
   *
   * @return std::string Contents of fd
   */
  virtual std::string read();
  void close() override;
  [[nodiscard]] bool closable() const override { return fd() >= 0; }
};

inline void idescriptor::close()
{
  if (closable())
  {
    ::close(fd());
    fd_ = -1;
  }
}

inline std::string idescriptor::read()
{
  static constexpr int buf_size{2048};
  static std::array<char, buf_size> buf;
  static std::string output;
  output.clear();
  ssize_t len;
  while ((len = ::read(fd(), buf.data(), 2048)) > 0)
  {
    output.append(buf.data(), len);
  }
  if (len < 0)
  {
    exceptions::os_error{"read"};
  }
  return output;
}

/**
 * @brief Wraps a descriptor mapping to a file on the disc
 *
 * The class exports the open() syscall and is the parent class of
 * ofile_descriptor and ifile_descriptor.
 *
 */
class file_descriptor : public virtual descriptor
{
public:
  file_descriptor(std::string path, int mode) : path_{std::move(path)}, mode_{mode} {}
  file_descriptor(const file_descriptor&)     = default;
  file_descriptor(file_descriptor&&) noexcept = default;
  file_descriptor& operator=(const file_descriptor&) = default;
  file_descriptor& operator=(file_descriptor&&) noexcept = default;
  ~file_descriptor() override { close(); }
  void open() override;

private:
  std::string path_;
  int mode_;
};

inline void file_descriptor::open()
{
  if (fd_ > 0)
  {
    return;
  }
  if (int fd{::open(path_.c_str(), mode_)}; fd >= 0)
  {
    fd_ = fd;
  }
  else
  {
    throw exceptions::os_error{"open", path_};
  }
}

/**
 * @brief Always opens the file in write-mode
 *
 */
class ofile_descriptor : public virtual odescriptor, public virtual file_descriptor
{
public:
  using file_descriptor::closable;
  using file_descriptor::close;
  explicit ofile_descriptor(std::string path, int mode = 0)
      : file_descriptor{std::move(path), O_WRONLY | mode}
  {
  }
};

/**
 * @brief Always opens the file in read-mode
 *
 */
class ifile_descriptor : public virtual idescriptor, public virtual file_descriptor
{
public:
  using file_descriptor::closable;
  using file_descriptor::close;
  explicit ifile_descriptor(std::string path, int mode = 0)
      : file_descriptor{std::move(path), O_RDONLY | mode}
  {
  }
};

class ipipe_descriptor;

/**
 * @brief A descriptor wrapping the output end of a posix OS pipe
 *
 * This class, when paired with ipipe_descriptor, consists of an OS
 * pipe. A pipe is constructed whenever open is called an object of
 * this type or its linked ipipe_descriptor. These should always be
 * constructed in pair with the subprocess::create_pipe method.
 */
class opipe_descriptor : public odescriptor
{
public:
  opipe_descriptor() = default;
  void open() override;

protected:
  ipipe_descriptor* linked_fd_{nullptr};
  friend class ipipe_descriptor;
  friend void link(ipipe_descriptor& fd1, opipe_descriptor& fd2);
};

/**
 * @brief A descriptor wrapping the input end of a posix OS pipe
 *
 * This class, when paired with opipe_descriptor, consists of an OS
 * pipe. A pipe is constructed whenever open is called an object of
 * this type or its linked opipe_descriptor. These should always be
 * constructed in pair with the subprocess::create_pipe method.
 */
class ipipe_descriptor : public idescriptor
{
public:
  ipipe_descriptor() = default;
  void open() override;

protected:
  opipe_descriptor* linked_fd_{nullptr};
  friend class opipe_descriptor;
  friend void link(ipipe_descriptor& fd1, opipe_descriptor& fd2);
};

inline void opipe_descriptor::open()
{
  if (closable())
  {
    return;
  }
  std::array<int, 2> fd{};
  if (::pipe(fd.data()) < 0)
  {
    throw exceptions::os_error{"pipe"};
  }
  linked_fd_->fd_ = fd[0];
  fd_             = fd[1];
}

inline void ipipe_descriptor::open()
{
  if (closable())
  {
    return;
  }
  std::array<int, 2> fd{};
  if (::pipe(fd.data()) < 0)
  {
    throw exceptions::os_error{"pipe"};
  }
  fd_             = fd[0];
  linked_fd_->fd_ = fd[1];
}

class ovariable_descriptor : public opipe_descriptor
{
public:
  explicit ovariable_descriptor(std::string& output_var) : output_{output_var} { linked_fd_ = &input_pipe_; }

  void close() override;
  virtual void read() { output_ = input_pipe_.read(); }

private:
  std::string& output_;
  ipipe_descriptor input_pipe_;
};

inline void ovariable_descriptor::close()
{
  if (not closable())
  {
    return;
  }
  opipe_descriptor::close();
  read();
  input_pipe_.close();
  fd_ = -1;
}
class ivariable_descriptor : public ipipe_descriptor
{
public:
  explicit ivariable_descriptor(std::string input_data) : input_{std::move(input_data)}
  {
    linked_fd_ = &output_pipe_;
  }

  void open() override;
  virtual void write() { output_pipe_.write(input_); }

private:
  std::string input_;
  opipe_descriptor output_pipe_;
};

inline void ivariable_descriptor::open()
{
  if (closable())
  {
    return;
  }
  ipipe_descriptor::open();
  write();
  output_pipe_.close();
}

/**
 * @brief Create opipe_descriptor and ipipe_descriptor objects and links them
 *
 * Calling this method does not automatically create a pipe. You need to call open() on
 * any one of the resulting objects to initialize the pipe.
 *
 * @return std::pair<descriptor_ptr_t<ipipe_descriptor>, descriptor_ptr_t<opipe_descriptor>> A pair of linked
 * file_descriptos [read_fd, write_fd]
 */
inline std::pair<descriptor_ptr_t<ipipe_descriptor>, descriptor_ptr_t<opipe_descriptor>> create_pipe()
{
  auto read_fd{make_descriptor<ipipe_descriptor>()};
  auto write_fd{make_descriptor<opipe_descriptor>()};
  link(*read_fd, *write_fd);
  return std::pair{std::move(read_fd), std::move(write_fd)};
}

/**
 * @brief Returns an abstraction of stdout file descriptor
 *
 * @return descriptor stdout
 */
inline descriptor_ptr std_in()
{
  static auto stdin_fd{
      make_descriptor<descriptor>(static_cast<int>(posix_util::standard_filenos::standard_in))};
  return stdin_fd;
};
/**
 * @brief Returns an abstraction of stdin file descriptor
 *
 * @return descriptor stdin
 */
inline descriptor_ptr std_out()
{
  static auto stdout_fd{
      make_descriptor<descriptor>(static_cast<int>(posix_util::standard_filenos::standard_out))};
  return stdout_fd;
};
/**
 * @brief Returns an abstraction of stderr file descriptor
 *
 * @return descriptor stderr
 */
inline descriptor_ptr std_err()
{
  static auto stderr_fd{
      make_descriptor<descriptor>(static_cast<int>(posix_util::standard_filenos::standard_error))};
  return stderr_fd;
};

/**
 * @brief Links two file descriptors
 *
 * This function is used to link a read and write file descriptor
 *
 * @param fd1
 * @param fd2
 */
inline void link(ipipe_descriptor& fd1, opipe_descriptor& fd2)
{
  if (fd1.linked_fd_ != nullptr or fd2.linked_fd_ != nullptr)
  {
    throw exceptions::usage_error{
        "You tried to link a file descriptor that is already linked to another file descriptor!"};
  }
  fd1.linked_fd_ = &fd2;
  fd2.linked_fd_ = &fd1;
}

namespace posix_util
{

/**
 * @brief A RAII wrapper over posix_spawn_file_actions_t
 *
 * posix_spawn_file_actions_t is used by the POSIX system call posix_spawnp
 * to decide what to do with the file descriptors after a child process is spawned.
 *
 * Any actions added to the object are performed sequentially in the child. If any action
 * is invalid or leads to an os_error, posix_spawnp errors out.
 *
 * This class manages the lifetime of the posix_spawn_file_actions_t objects so
 * that it is difficult for the user to leak memory. It also exports some helper
 * functions to add actions to the encapsulated posix_spawn_file_actions_t struct.
 */
class posix_spawn_file_actions
{
public:
  posix_spawn_file_actions()
  {
    posix_spawn_file_actions_init(&actions_);
    closed_fds_.reserve(3);
  }
  posix_spawn_file_actions(const posix_spawn_file_actions&)     = default;
  posix_spawn_file_actions(posix_spawn_file_actions&&) noexcept = default;
  posix_spawn_file_actions& operator=(const posix_spawn_file_actions&) = default;
  posix_spawn_file_actions& operator=(posix_spawn_file_actions&&) noexcept = default;
  ~posix_spawn_file_actions() { posix_spawn_file_actions_destroy(&actions_); }

  void dup(const descriptor_ptr& fd_from, standard_filenos fd_to)
  {
    posix_spawn_file_actions_adddup2(&actions_, fd_from->fd(), static_cast<int>(fd_to));
  }
  void close(const descriptor_ptr& fd)
  {
    if (fd->closable() and closed_fds_.find(fd->fd()) == closed_fds_.end())
    {
      posix_spawn_file_actions_addclose(&actions_, fd->fd());
      closed_fds_.insert(fd->fd());
    }
  }

  posix_spawn_file_actions_t* get() { return &actions_; }

private:
  posix_spawn_file_actions_t actions_{};
  std::unordered_set<int> closed_fds_{};
};
} // namespace posix_util

/**
 * @brief Abstraction of a POSIX process with stdin, stdout, and stderr.
 *
 * This class encapsulates a POSIX process. It is the actual interface for
 * running subprocesses. The descriptors in this class can be pointed to
 * each other, and to other opened descriptors.
 *
 * posix::spawnp is used to spawn child processes. The following actions
 * are performed on descriptors on execution:
 *
 *  - All descriptors are opened with a call to descriptor::open().
 *  - descriptor::fd() is used to get descriptors for the child process and
 *     dup them to stdin, stdout, and stderr
 *  - In the child process, the descriptors returned by descriptor::fd() would
 *    be closed.
 *  - After the process is spawned, the parent process closes each descriptor with
 *    a call to descriptor::close.
 *
 * Therefore, while providing your own descriptors, it is important to correctly implement
 * open(), fd(), and close() calls to the class that derives from subprocess::descriptor.
 */
class posix_process
{

public:
  explicit posix_process(std::string cmd) : cmd_{std::move(cmd)} {}

  void execute();

  int wait();

  const descriptor& in() { return *stdin_fd_; }

  const descriptor& out() { return *stdout_fd_; }

  const descriptor& err() { return *stderr_fd_; }

  void in(descriptor_ptr&& fd) { stdin_fd_ = std::move(fd); }
  void out(descriptor_ptr&& fd) { stdout_fd_ = std::move(fd); }
  void err(descriptor_ptr&& fd) { stderr_fd_ = std::move(fd); }
  void out(err_t /*unused*/) { stdout_fd_ = stderr_fd_; }
  void err(out_t /*unused*/) { stderr_fd_ = stdout_fd_; }

private:
  std::string cmd_;
  descriptor_ptr stdin_fd_{subprocess::std_in()}, stdout_fd_{subprocess::std_out()},
      stderr_fd_{subprocess::std_err()};
  std::optional<int> pid_;
};

/**
 * @brief Spawns the child process
 *
 * This method performs shell expansion, manages the file descriptors,
 * and ensures that the child process is spawned with the correct
 * descriptors.
 *
 * It does NOT reap the child process from the process table.
 *
 */
inline void posix_process::execute()
{
  auto process_fds = [](posix_util::posix_spawn_file_actions& action, descriptor_ptr& fd,
                        posix_util::standard_filenos dup_to)
  {
    fd->open();
    action.dup(fd, dup_to);
  };

  posix_util::shell_expander sh{cmd_};
  posix_util::posix_spawn_file_actions action;

  process_fds(action, stdin_fd_, posix_util::standard_filenos::standard_in);
  process_fds(action, stdout_fd_, posix_util::standard_filenos::standard_out);
  process_fds(action, stderr_fd_, posix_util::standard_filenos::standard_error);
  action.close(stdin_fd_);
  action.close(stdout_fd_);
  action.close(stderr_fd_);

  int pid{};
  if (int err{::posix_spawnp(&pid, sh.argv()[0], action.get(), nullptr, sh.argv(), nullptr)}; err != 0)
  {
    throw exceptions::os_error{{"posix_spawnp:", sh.argv()[0]}, err};
  }
  pid_ = pid;
  stdin_fd_->close();
  stdout_fd_->close();
  stderr_fd_->close();
}

/**
 * @brief Reaps the child process from the process table
 *
 * @return int exit status of the process
 */
inline int posix_process::wait()
{
  if (not pid_)
  {
    throw exceptions::usage_error{"posix_process.wait() called before posix_process.execute()"};
  }
  int waitstatus{};
  ::waitpid(*(pid_), &waitstatus, 0);
  return WEXITSTATUS(waitstatus);
}

using process_t = posix_process;

/**
 * @brief Main interface class for subprocess library
 *
 * A class that contains a list of linked shell commands and is responsible
 * for managing their file descriptors such that their input and output can
 * be chained to other commands.
 *
 */
class command
{
public:
  explicit command(std::string cmd) { processes_.emplace_back(std::move(cmd)); }

  /**
   * @brief Runs the command pipeline and throws on error.
   *
   * run only returns when the return code from the pipeline is 0.
   * Otherwise, it throws subprocess::exceptions::command_error.
   *
   * It can also throw exceptions::os_error if the command could not be
   * run due to some operating system level restrictions/errors.
   *
   * @return int Return code from the pipeline
   */
  int run();

  /**
   * @brief Runs the command pipeline and doesn't throw.
   *
   * run(nothrow_t) returns the exit code from the pipeline.
   * It doesn't throw subprocess::exceptions::command_error.
   *
   * However, it can still throw exceptions::os_error in case of
   * operating system level restrictions/errors.
   *
   * @return int Return code from the pipeline
   */
  int run(std::nothrow_t /*unused*/);

  /**
   * @brief Chains a command object to the current one.
   *
   * The output of the current command object is piped into
   * the other command object.
   *
   * Currently, the argument can only be an r-value because of
   * open file descriptors. This will be changed soon.
   *
   * @param other Command ob
   * @return command&
   */
  command& operator|(command&& other);
  command& operator|(std::string other);

private:
  std::list<process_t> processes_;

  friend command& operator<(command& cmd, descriptor_ptr fd);

  friend command& operator>(command& cmd, err_t err_tag);
  friend command& operator>(command& cmd, descriptor_ptr fd);

  friend command& operator>=(command& cmd, out_t out_tag);
  friend command& operator>=(command& cmd, descriptor_ptr fd);
};

inline int command::run(std::nothrow_t /*unused*/)
{
  for (auto& process : processes_)
  {
    process.execute();
  }
  int waitstatus{};
  for (auto& process : processes_)
  {
    waitstatus = process.wait();
  }
  return waitstatus;
}

inline int command::run()
{
  int status{run(std::nothrow)};
  if (status != 0)
  {
    throw exceptions::command_error{{"command exitstatus", std::to_string(status)}, status};
  }
  return status;
}

inline command& command::operator|(command&& other)
{
  auto [read_fd, write_fd] = create_pipe();
  other.processes_.front().in(std::move(read_fd));
  processes_.back().out(std::move(write_fd));
  processes_.splice(processes_.end(), std::move(other.processes_));
  return *this;
}

inline command& command::operator|(std::string other) { return *this | command{std::move(other)}; }

inline command& operator<(command& cmd, descriptor_ptr fd)
{
  cmd.processes_.front().in(std::move(fd));
  return cmd;
}

inline command& operator>(command& cmd, err_t err_tag)
{
  cmd.processes_.back().out(err_tag);
  return cmd;
}

inline command& operator>(command& cmd, descriptor_ptr fd)
{
  cmd.processes_.back().out(std::move(fd));
  return cmd;
}

inline command& operator>=(command& cmd, out_t out_tag)
{
  cmd.processes_.back().err(out_tag);
  return cmd;
}

inline command& operator>=(command& cmd, descriptor_ptr fd)
{
  cmd.processes_.back().err(std::move(fd));
  return cmd;
}

inline command& operator>>(command& cmd, descriptor_ptr fd) { return (cmd > std::move(fd)); }

inline command& operator>>=(command& cmd, descriptor_ptr fd) { return (cmd >= std::move(fd)); }

inline command& operator>=(command& cmd, std::string& output)
{
  return cmd >= make_descriptor<ovariable_descriptor>(output);
}

inline command& operator>(command& cmd, std::string& output)
{
  return cmd > make_descriptor<ovariable_descriptor>(output);
}

inline command& operator<(command& cmd, std::string& input)
{
  return cmd < make_descriptor<ivariable_descriptor>(input);
}

inline command& operator>(command& cmd, std::filesystem::path file_name)
{
  return cmd > make_descriptor<ofile_descriptor>(file_name.string(), O_CREAT | O_TRUNC);
}

inline command& operator>=(command& cmd, std::filesystem::path file_name)
{
  return cmd >= make_descriptor<ofile_descriptor>(file_name.string(), O_CREAT | O_TRUNC);
}

inline command& operator>>(command& cmd, std::filesystem::path file_name)
{
  return cmd >> make_descriptor<ofile_descriptor>(file_name.string(), O_CREAT | O_APPEND);
}

inline command& operator>>=(command& cmd, std::filesystem::path file_name)
{
  return cmd >>= make_descriptor<ofile_descriptor>(file_name.string(), O_CREAT | O_APPEND);
}

inline command& operator<(command& cmd, std::filesystem::path file_name)
{
  return cmd < make_descriptor<ofile_descriptor>(file_name.string());
}

inline command&& operator>(command&& cmd, descriptor_ptr fd) { return std::move(cmd > std::move(fd)); }

inline command&& operator>=(command&& cmd, descriptor_ptr fd) { return std::move(cmd >= std::move(fd)); }

inline command&& operator>>(command&& cmd, descriptor_ptr fd) { return std::move(cmd >> std::move(fd)); }

inline command&& operator>>=(command&& cmd, descriptor_ptr fd) { return std::move(cmd >>= std::move(fd)); }

inline command&& operator<(command&& cmd, descriptor_ptr fd) { return std::move(cmd < std::move(fd)); }

inline command&& operator>=(command&& cmd, out_t out_tag) { return std::move(cmd >= out_tag); }

inline command&& operator>=(command&& cmd, std::string& output) { return std::move(cmd >= output); }

inline command&& operator>(command&& cmd, err_t err_tag) { return std::move(cmd > err_tag); }

inline command&& operator>(command&& cmd, std::string& output) { return std::move(cmd > output); }

inline command&& operator<(command&& cmd, std::string& input) { return std::move(cmd < input); }

inline command&& operator>(command&& cmd, std::filesystem::path file_name)
{
  return std::move(cmd > std::move(file_name));
}

inline command&& operator>=(command&& cmd, std::filesystem::path file_name)
{
  return std::move(cmd >= std::move(file_name));
}
inline command&& operator>>(command&& cmd, std::filesystem::path file_name)
{
  return std::move(cmd >> std::move(file_name));
}

inline command&& operator>>=(command&& cmd, std::filesystem::path file_name)
{
  return std::move(cmd >>= std::move(file_name));
}

inline command&& operator<(command&& cmd, std::filesystem::path file_name)
{
  return std::move(cmd < std::move(file_name));
}

inline namespace literals
{
inline command operator""_cmd(const char* cmd, size_t /*unused*/) { return command{cmd}; }
} // namespace literals

} // namespace subprocess
