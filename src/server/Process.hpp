#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace cfalsp {

class CancelToken {
  public:
	void cancel() { flag = true; }
	bool cancelled() const { return flag; }
  private:
	std::atomic<bool> flag{ false };
};

struct RunOptions {
	std::string cwd;						// empty: inherit
	std::string stdoutPath;					// empty: /dev/null
	std::string stderrPath;					// empty: /dev/null
	std::chrono::milliseconds timeout{ 0 };	// 0: none
	std::vector<std::pair<std::string, std::string>> env;	// added to / overriding the environment
};

struct RunResult {
	enum Status { Exited, Signaled, TimedOut, Cancelled, SpawnFailed } status = SpawnFailed;
	int code = 0;							// exit code or signal number
	std::string error;						// for SpawnFailed
	bool ok() const { return status == Exited && code == 0; }
	std::string describe() const;
};

// Runs argv[0] (looked up in PATH if it has no '/') in its own process
// group with stdin from /dev/null. On timeout or cancellation the whole
// group is killed with SIGKILL and the child is reaped before returning.
RunResult runProcess( const std::vector<std::string> & argv, const RunOptions & opts,
					  const CancelToken * cancel = nullptr );

// The first executable called `name` in PATH, or "" if none.
std::string findInPath( const std::string & name );

} // namespace cfalsp
