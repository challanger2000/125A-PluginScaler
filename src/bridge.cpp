#include "scaler/bridge.hpp"
#include <cmath>

namespace scaler {
std::wstring moduleDirectory(HMODULE module) {
  wchar_t path[32768]{};
  auto n = GetModuleFileNameW(module, path, 32768);
  if (!n || n >= 32768)
    return {};
  std::wstring s(path);
  return s.substr(0, s.find_last_of(L"\\/"));
}
bool Bridge::open(const std::wstring &dir, const std::wstring &target,
                  const std::wstring &helper) {
  static volatile LONG counter = 0;
  auto name = L"Local\\125A-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
              std::to_wstring(InterlockedIncrement(&counter));
  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, sizeof(Shared), name.c_str());
  if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS)
    return false;
  shared = static_cast<Shared *>(
      MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  if (!shared)
    return false;
  ZeroMemory(shared, sizeof(Shared));
  shared->version = protocolVersion;
  shared->bytes = sizeof(Shared);
  shared->sampleRate = 48000;
  request_ = CreateEventW(nullptr, FALSE, FALSE, (name + L"-request").c_str());
  done_ = CreateEventW(nullptr, FALSE, FALSE, (name + L"-done").c_str());
  if (!request_ || !done_)
    return false;
  // Job lifetime follows the instance, including a hung or crashed editor.
  job_ = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits)))
    return false;
  std::wstring exe = dir + L"\\" + helper;
  std::wstring command =
      L"\"" + exe + L"\" \"" + name + L"\" \"" + target + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, dir.c_str(),
                      &si, &pi))
    return false;
  process_ = pi.hProcess;
  if (!AssignProcessToJobObject(job_, process_)) {
    TerminateProcess(process_, 1);
    CloseHandle(pi.hThread);
    return false;
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  HANDLE handles[]{done_, process_};
  if (WaitForMultipleObjects(2, handles, FALSE, 10000) != WAIT_OBJECT_0 ||
      shared->ready != 1) {
    disconnect();
    return false;
  }
  return true;
}
bool Bridge::transact(Op op, DWORD timeout) {
  if (!healthy() || InterlockedCompareExchange(&inFlight_, 1, 0) != 0)
    return false;
  ResetEvent(done_);
  InterlockedExchange(&shared->operation, static_cast<LONG>(op));
  SetEvent(request_);
  HANDLE handles[]{done_, process_};
  bool completed =
      WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0;
  bool ok = completed && shared->result == 1;
  if (!completed || (!ok && op == Op::Audio))
    disconnect(); // Never reuse a timed-out slot or replay its MIDI.
  InterlockedExchange(&inFlight_, 0);
  return ok;
}
bool Bridge::configure(double rate) {
  std::lock_guard<std::mutex> lock(control_);
  if (!shared || !std::isfinite(rate) || rate < 8000 || rate > 384000)
    return false;
  shared->sampleRate = rate;
  return transact(Op::Configure, 2000);
}
bool Bridge::audio(uint32_t frames, bool offline) {
  if (!shared || frames > maxFrames)
    return false;
  shared->frames = frames;
  shared->offline = offline;
  auto ms = static_cast<DWORD>(
      std::max(1.0, std::ceil(1000.0 * frames / shared->sampleRate)));
  return transact(Op::Audio, offline ? 5000 : ms);
}
bool Bridge::activate(bool state) {
  std::lock_guard<std::mutex> lock(control_);
  if (!shared)
    return false;
  shared->offline = state ? 1 : 0;
  return transact(Op::Activate, 2000);
}
bool Bridge::state(bool restore) {
  std::lock_guard<std::mutex> lock(control_);
  return transact(restore ? Op::SetState : Op::GetState, 2000);
}
bool Bridge::input(Input in) {
  if (!healthy())
    return false;
  LONG w = InterlockedCompareExchange(&shared->inputWrite, 0, 0),
       r = InterlockedCompareExchange(&shared->inputRead, 0, 0);
  if (uint32_t(w - r) >= 512) {
    InterlockedIncrement(&shared->inputDrops);
    return false;
  }
  shared->input[uint32_t(w) % 512] = in;
  InterlockedExchange(&shared->inputWrite, LONG(uint32_t(w) + 1));
  return true;
}
Bridge::~Bridge() {
  if (shared) {
    InterlockedExchange(&shared->stopping, 1);
    if (request_)
      SetEvent(request_);
  }
  if (process_ && WaitForSingleObject(process_, 1000) == WAIT_TIMEOUT)
    TerminateJobObject(job_, 1);
  if (process_) {
    WaitForSingleObject(process_, 1000);
    CloseHandle(process_);
  }
  if (job_)
    CloseHandle(job_);
  if (shared)
    UnmapViewOfFile(shared);
  if (mapping_)
    CloseHandle(mapping_);
  if (request_)
    CloseHandle(request_);
  if (done_)
    CloseHandle(done_);
}
} // namespace scaler
