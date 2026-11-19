#include "optum/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "optum/ir.hpp"

namespace optum {
namespace {

void close_fd(int& fd) {
  if (fd >= 0) close(fd);
  fd = -1;
}

}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv, const std::string& input,
                          std::chrono::milliseconds timeout) {
  if (argv.empty()) throw IrError("empty command");
  int in[2], out[2], err[2];
  if (pipe(in) || pipe(out) || pipe(err)) throw IrError(std::string("pipe: ") + std::strerror(errno));
  // A pipe to report exec failure: closed on successful exec (O_CLOEXEC), written on failure.
  int status_pipe[2];
  if (pipe2(status_pipe, O_CLOEXEC)) throw IrError(std::string("pipe: ") + std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) throw IrError(std::string("fork: ") + std::strerror(errno));
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(err[1], 2);
    for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1], status_pipe[0]}) close(fd);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    const int e = errno;
    (void)!write(status_pipe[1], &e, sizeof e);
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  close(err[1]);
  close(status_pipe[1]);
  int exec_errno = 0;
  if (read(status_pipe[0], &exec_errno, sizeof exec_errno) == sizeof exec_errno) {
    close(status_pipe[0]);
    close(in[1]);
    close(out[0]);
    close(err[0]);
    waitpid(pid, nullptr, 0);
    throw IrError("cannot run '" + argv[0] + "': " + std::strerror(exec_errno));
  }
  close(status_pipe[0]);

  int fd_in = in[1], fd_out = out[0], fd_err = err[0];
  fcntl(fd_in, F_SETFL, O_NONBLOCK);
  signal(SIGPIPE, SIG_IGN);  // a child that exits early must not kill us
  if (input.empty()) close_fd(fd_in);

  ProcessResult r;
  std::size_t written = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  char buf[65536];
  while (fd_out >= 0 || fd_err >= 0) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      r.timed_out = true;
      kill(pid, SIGKILL);
      break;
    }
    pollfd fds[3];
    nfds_t n = 0;
    if (fd_in >= 0) fds[n++] = {fd_in, POLLOUT, 0};
    if (fd_out >= 0) fds[n++] = {fd_out, POLLIN, 0};
    if (fd_err >= 0) fds[n++] = {fd_err, POLLIN, 0};
    if (poll(fds, n, static_cast<int>(std::min<long long>(left.count(), 1000))) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (nfds_t i = 0; i < n; ++i) {
      if (!fds[i].revents) continue;
      if (fds[i].fd == fd_in) {
        const ssize_t w = write(fd_in, input.data() + written, input.size() - written);
        if (w > 0) written += static_cast<std::size_t>(w);
        if (w < 0 && errno != EAGAIN) close_fd(fd_in);
        if (written == input.size()) close_fd(fd_in);
      } else {
        int& fd = fds[i].fd == fd_out ? fd_out : fd_err;
        std::string& dst = fds[i].fd == fd_out ? r.out : r.err;
        const ssize_t got = read(fd, buf, sizeof buf);
        if (got > 0) dst.append(buf, static_cast<std::size_t>(got));
        else if (got == 0 || errno != EAGAIN) close_fd(fd);
      }
    }
  }
  close_fd(fd_in);
  close_fd(fd_out);
  close_fd(fd_err);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!r.timed_out && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  return r;
}

std::vector<std::string> split_command(const std::string& command) {
  std::vector<std::string> out;
  std::string cur;
  bool in_word = false;
  char quote = 0;
  for (char c : command) {
    if (quote) {
      if (c == quote) quote = 0;
      else cur += c;
    } else if (c == '\'' || c == '"') {
      quote = c;
      in_word = true;
    } else if (c == ' ' || c == '\t') {
      if (in_word) out.push_back(cur);
      cur.clear();
      in_word = false;
    } else {
      cur += c;
      in_word = true;
    }
  }
  if (quote) throw IrError("unclosed quote in command: " + command);
  if (in_word) out.push_back(cur);
  return out;
}

}  // namespace optum
