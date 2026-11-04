//  Copyright (c) 2026-present, Samsung Electronics.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "util/write_streams.h"

#ifdef OS_LINUX
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <list>
#include <mutex>

#include "logging/logging.h"

namespace ROCKSDB_NAMESPACE {

#ifdef OS_LINUX

// From include/uapi/linux/fs.h; defined here so that older kernel headers
// still build.
#ifndef FS_IOC_WRITE_STREAM_GET_MAX
struct fs_write_stream_set {
  int32_t stream_fd;  // stream to set, -1 to clear
  uint32_t flags;     // FS_WRITE_STREAM_SET_*
};
#define FS_WRITE_STREAM_SET_CLEAR (1U << 0)
#define FS_IOC_WRITE_STREAM_GET_MAX _IOR(0x15, 3, uint32_t)
#define FS_IOC_WRITE_STREAM_ALLOC _IO(0x15, 4)
#define FS_IOC_WRITE_STREAM_SET _IOW(0x15, 5, struct fs_write_stream_set)
#endif

namespace {

// One stream per lifetime class, indexed by ClassOf().
constexpr int kNumStreams = 4;

// Failures logged one by one; later ones are only counted.
constexpr uint64_t kLoggedFailures = 16;

int ClassOf(Env::WriteLifeTimeHint hint) {
  switch (hint) {
    case Env::WLTH_SHORT:
      return 0;
    case Env::WLTH_MEDIUM:
      return 1;
    case Env::WLTH_LONG:
      return 2;
    case Env::WLTH_EXTREME:
      return 3;
    default:
      return -1;
  }
}

// The streams of one filesystem.
struct StreamSet {
  uint64_t dev = 0;
  bool active = false;  // false if allocation failed
  int fds[kNumStreams] = {-1, -1, -1, -1};
  // One entry per reference; failures are logged to the first.
  std::vector<std::shared_ptr<Logger>> loggers;
  uint64_t attached[kNumStreams] = {0, 0, 0, 0};
  uint64_t detached = 0;
  uint64_t failed = 0;
};

std::mutex mu;
std::list<StreamSet> sets;
// Files on a filesystem no DB acquired streams for.
uint64_t unknown_fs = 0;

StreamSet* Find(uint64_t dev) {
  for (auto& s : sets) {
    if (s.dev == dev) {
      return &s;
    }
  }
  return nullptr;
}

void CloseStreams(StreamSet* s) {
  for (int& fd : s->fds) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
  s->active = false;
}

// REQUIRES: mu held
void AllocStreams(StreamSet* s, const std::string& dir, Logger* info_log) {
  int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) {
    ROCKS_LOG_WARN(info_log, "write streams: open %s: %s", dir.c_str(),
                   strerror(errno));
    return;
  }

  uint32_t max = 0;
  if (ioctl(dfd, FS_IOC_WRITE_STREAM_GET_MAX, &max) < 0) {
    ROCKS_LOG_WARN(info_log, "write streams: GET_MAX on %s: %s", dir.c_str(),
                   strerror(errno));
    close(dfd);
    return;
  }
  if (max < kNumStreams) {
    ROCKS_LOG_WARN(info_log,
                   "write streams: %s offers %u streams, need %d; disabled",
                   dir.c_str(), max, kNumStreams);
    close(dfd);
    return;
  }

  for (int i = 0; i < kNumStreams; i++) {
    int fd = ioctl(dfd, FS_IOC_WRITE_STREAM_ALLOC);
    if (fd < 0) {
      ROCKS_LOG_WARN(info_log, "write streams: ALLOC on %s: %s", dir.c_str(),
                     strerror(errno));
      CloseStreams(s);
      close(dfd);
      return;
    }
    s->fds[i] = fd;
  }
  close(dfd);

  s->active = true;
  ROCKS_LOG_INFO(info_log, "write streams: %d of %u allocated on %s",
                 kNumStreams, max, dir.c_str());
}

// REQUIRES: mu held
Logger* AnyLogger() {
  for (auto& s : sets) {
    if (!s.loggers.empty()) {
      return s.loggers.front().get();
    }
  }
  return nullptr;
}

}  // namespace

void WriteStreamsAcquire(const std::vector<std::string>& dirs,
                         const std::shared_ptr<Logger>& info_log,
                         std::vector<uint64_t>* devs) {
  std::lock_guard<std::mutex> lock(mu);

  for (auto& dir : dirs) {
    struct stat st;
    if (stat(dir.c_str(), &st) < 0) {
      ROCKS_LOG_WARN(info_log, "write streams: stat %s: %s", dir.c_str(),
                     strerror(errno));
      continue;
    }
    uint64_t dev = static_cast<uint64_t>(st.st_dev);
    if (std::find(devs->begin(), devs->end(), dev) != devs->end()) {
      continue;
    }

    StreamSet* s = Find(dev);
    if (s == nullptr) {
      sets.emplace_back();
      s = &sets.back();
      s->dev = dev;
      AllocStreams(s, dir, info_log.get());
    }
    // Keep inactive sets too, so their files are counted as failures.
    s->loggers.push_back(info_log);
    devs->push_back(dev);
  }
}

void WriteStreamsRelease(const std::vector<uint64_t>& devs,
                         const std::shared_ptr<Logger>& info_log) {
  std::lock_guard<std::mutex> lock(mu);

  for (uint64_t dev : devs) {
    StreamSet* s = Find(dev);
    if (s == nullptr) {
      continue;
    }
    ROCKS_LOG_INFO(info_log,
                   "write streams: dev %" PRIu64 " %s: attached short=%" PRIu64
                   " medium=%" PRIu64 " long=%" PRIu64 " extreme=%" PRIu64
                   " detached=%" PRIu64 " failed=%" PRIu64,
                   dev, s->active ? "active" : "inactive", s->attached[0],
                   s->attached[1], s->attached[2], s->attached[3], s->detached,
                   s->failed);

    auto it = std::find(s->loggers.begin(), s->loggers.end(), info_log);
    if (it != s->loggers.end()) {
      s->loggers.erase(it);
    }
    if (s->loggers.empty()) {
      CloseStreams(s);
      sets.remove_if([dev](const StreamSet& x) { return x.dev == dev; });
    }
  }
  if (unknown_fs) {
    ROCKS_LOG_WARN(info_log,
                   "write streams: %" PRIu64
                   " files on filesystems without streams",
                   unknown_fs);
    unknown_fs = 0;
  }
}

bool WriteStreamsSet(int fd, Env::WriteLifeTimeHint hint) {
  std::lock_guard<std::mutex> lock(mu);
  int c = ClassOf(hint);

  struct stat st;
  if (fstat(fd, &st) < 0) {
    return false;
  }
  StreamSet* s = Find(static_cast<uint64_t>(st.st_dev));
  if (s == nullptr) {
    // Only a problem for files that should have been placed.
    if (c >= 0 && unknown_fs++ < kLoggedFailures) {
      ROCKS_LOG_WARN(AnyLogger(),
                     "write streams: inode %" PRIu64 " on dev %" PRIu64
                     " (hint %d): no streams for this filesystem",
                     static_cast<uint64_t>(st.st_ino),
                     static_cast<uint64_t>(st.st_dev), static_cast<int>(hint));
    }
    // Nothing can be attached here, so nothing to detach either.
    return c < 0;
  }

  struct fs_write_stream_set set;
  if (c >= 0) {
    if (!s->active) {
      s->failed++;
      return false;
    }
    set.stream_fd = s->fds[c];
    set.flags = 0;
  } else {
    if (!s->active) {
      return true;
    }
    set.stream_fd = -1;
    set.flags = FS_WRITE_STREAM_SET_CLEAR;
  }

  if (ioctl(fd, FS_IOC_WRITE_STREAM_SET, &set) < 0) {
    if (s->failed++ < kLoggedFailures) {
      Logger* log = s->loggers.empty() ? nullptr : s->loggers.front().get();
      ROCKS_LOG_WARN(log,
                     "write streams: SET on inode %" PRIu64 " (hint %d): %s",
                     static_cast<uint64_t>(st.st_ino), static_cast<int>(hint),
                     strerror(errno));
    }
    return false;
  }
  if (c >= 0) {
    s->attached[c]++;
  } else {
    s->detached++;
  }
  return true;
}

#else  // OS_LINUX

void WriteStreamsAcquire(const std::vector<std::string>& /*dirs*/,
                         const std::shared_ptr<Logger>& /*info_log*/,
                         std::vector<uint64_t>* /*devs*/) {}

void WriteStreamsRelease(const std::vector<uint64_t>& /*devs*/,
                         const std::shared_ptr<Logger>& /*info_log*/) {}

bool WriteStreamsSet(int /*fd*/, Env::WriteLifeTimeHint /*hint*/) {
  return true;
}

#endif  // OS_LINUX

}  // namespace ROCKSDB_NAMESPACE
