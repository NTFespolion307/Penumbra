// penumbra-nettest: runs several processes at once for the network tests (a controller and its workers),
// each with its output in <workdir>/<name>.out, and checks their exit codes.
//
//   penumbra-nettest --workdir DIR [--timeout SECONDS]
//       --proc NAME DELAY_MS EXPECT KILL_AFTER_MS "exe|arg|arg..." [--proc ...]
//
// DELAY_MS: start this long after the first process, or "after:NAME" to start once process NAME has ended.
// EXPECT: the exit code it must return, "any", or "daemon" (a helper such as a proxy: stopped once every other
// process has ended).
// KILL_AFTER_MS: hard-kill it this long after it started (0 = never); a killed process passes when EXPECT is
// "killed". Exit code 0 when every process ends as expected within the timeout, 1 otherwise.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Proc {
  std::string name;
  int delay_ms = 0;
  std::string after;  // start once this process has ended
  std::string expect;
  int kill_after_ms = 0;
  std::vector<std::string> argv;

  bool started = false, done = false, killed = false;
  int exit_code = -1;
  Clock::time_point start;
#if defined(_WIN32)
  HANDLE process = nullptr;
#else
  pid_t pid = -1;
#endif
};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Quotes one argument the way CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out += c;
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      out += c;
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}

bool startProc(Proc& p, const fs::path& out) {
  std::wstring cmd;
  for (const auto& a : p.argv) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE file = CreateFileW(out.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = file;
  si.hStdError = file;
  // No terminal input: a process that asks a question (--approve-workers) reads end-of-input.
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  si.hStdInput = nul;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  CloseHandle(file);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  p.process = pi.hProcess;
  return true;
}

bool poll(Proc& p) {
  if (WaitForSingleObject(p.process, 0) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(p.process, &code);
  p.exit_code = static_cast<int>(code);
  CloseHandle(p.process);
  return true;
}

void hardKill(Proc& p) { TerminateProcess(p.process, 137); }
#else
bool startProc(Proc& p, const fs::path& out) {
  const int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
    const int nul = open("/dev/null", O_RDONLY);  // no terminal input (see the Windows branch)
    if (nul >= 0) dup2(nul, 0);
    std::vector<char*> args;
    for (auto& a : p.argv) args.push_back(a.data());
    args.push_back(nullptr);
    execv(args[0], args.data());
    std::perror("execv");
    _exit(127);
  }
  close(fd);
  p.pid = pid;
  return true;
}

bool poll(Proc& p) {
  int status = 0;
  if (waitpid(p.pid, &status, WNOHANG) != p.pid) return false;
  p.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return true;
}

void hardKill(Proc& p) { kill(p.pid, SIGKILL); }
#endif

void printTail(const fs::path& file, int lines) {
  std::ifstream in(file);
  std::vector<std::string> all;
  for (std::string l; std::getline(in, l);) all.push_back(l);
  const size_t from = all.size() > size_t(lines) ? all.size() - lines : 0;
  for (size_t i = from; i < all.size(); ++i) std::cout << "    " << all[i] << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  fs::path workdir = ".";
  int timeout = 600;
  std::vector<Proc> procs;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--workdir" && i + 1 < argc) {
      workdir = argv[++i];
    } else if (a == "--timeout" && i + 1 < argc) {
      timeout = std::atoi(argv[++i]);
    } else if (a == "--proc" && i + 5 < argc) {
      Proc p;
      p.name = argv[++i];
      const std::string d = argv[++i];
      if (d.rfind("after:", 0) == 0) p.after = d.substr(6);
      else p.delay_ms = std::atoi(d.c_str());
      p.expect = argv[++i];
      p.kill_after_ms = std::atoi(argv[++i]);
      p.argv = split(argv[++i], '|');
      procs.push_back(std::move(p));
    } else {
      std::cerr << "penumbra-nettest: unexpected argument '" << a << "'\n";
      return 2;
    }
  }
  if (procs.empty()) {
    std::cerr << "penumbra-nettest: no --proc given\n";
    return 2;
  }

  const auto t0 = Clock::now();
  const auto ms = [](Clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
  bool timedOut = false;
  for (;;) {
    const auto now = Clock::now();
    bool allDone = true;
    for (auto& p : procs) {
      bool ready = ms(now - t0) >= p.delay_ms;
      if (!p.after.empty()) {
        ready = false;
        for (const auto& q : procs)
          if (q.name == p.after) ready = q.done;
      }
      if (!p.started && ready) {
        if (!startProc(p, workdir / (p.name + ".out"))) {
          std::cerr << "cannot start " << p.name << " (" << p.argv[0] << ")\n";
          p.done = true;
          p.exit_code = -2;
          p.started = true;
          continue;
        }
        p.started = true;
        p.start = now;
        std::cout << "[" << ms(now - t0) << " ms] started " << p.name << "\n";
      }
      if (p.started && !p.done) {
        if (poll(p)) {
          p.done = true;
          std::cout << "[" << ms(Clock::now() - t0) << " ms] " << p.name << " exited with " << p.exit_code
                    << (p.killed ? " (killed)" : "") << "\n";
        } else if (p.kill_after_ms > 0 && !p.killed && ms(now - p.start) >= p.kill_after_ms) {
          std::cout << "[" << ms(now - t0) << " ms] killing " << p.name << "\n";
          hardKill(p);
          p.killed = true;
        }
      }
      allDone = allDone && p.done;
    }
    // Helpers ("daemon") are stopped once everything else has ended.
    bool othersDone = true;
    for (const auto& p : procs) othersDone = othersDone && (p.expect == "daemon" || p.done);
    if (othersDone && !allDone)
      for (auto& p : procs)
        if (p.expect == "daemon" && p.started && !p.done && !p.killed) {
          hardKill(p);
          p.killed = true;
        }
    if (allDone) break;
    if (ms(now - t0) > timeout * 1000LL) {
      timedOut = true;
      std::cout << "timeout after " << timeout << " s; killing what is left\n";
      for (auto& p : procs)
        if (p.started && !p.done) {
          hardKill(p);
          while (!poll(p)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
          p.done = true;
          p.exit_code = -1;
        }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  bool ok = !timedOut;
  for (const auto& p : procs) {
    bool pass;
    if (p.expect == "any" || p.expect == "daemon") pass = true;
    else if (p.expect == "killed") pass = p.killed;
    else pass = !p.killed && p.exit_code == std::atoi(p.expect.c_str());
    std::cout << (pass ? "  ok   " : "  FAIL ") << p.name << ": exit " << p.exit_code << (p.killed ? " (killed)" : "")
              << ", expected " << p.expect << "\n";
    if (!pass) {
      ok = false;
      printTail(workdir / (p.name + ".out"), 40);
    }
  }
  return ok ? 0 : 1;
}
