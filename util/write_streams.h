//  Copyright (c) 2026-present, Samsung Electronics.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Linux write streams (FS_IOC_WRITE_STREAM_*) for lifetime-based placement.
//
// DB::Open() allocates one write stream per lifetime class on every
// filesystem the DB writes to (DB directory, WAL directory, db_paths and
// cf_paths):
//
//   WLTH_SHORT    WAL
//   WLTH_MEDIUM   flush and SSTs up to base_level + 2
//   WLTH_LONG     SSTs at base_level + 3
//   WLTH_EXTREME  SSTs at base_level + 4 and below
//
// (L0-L3 / L4 / L5+ with the usual base_level of 1.)  Blob files, when
// enabled, get the hint of the level they are written for.
//
// A file given one of these hints is attached to the matching stream of the
// filesystem it lives on; a file whose hint changes to one with no stream is
// detached.  Files with no hint (MANIFEST, CURRENT, OPTIONS, LOG, IDENTITY,
// temporary files) are left alone and use default placement.
//
// Stream ids are opaque: which ids the kernel hands out does not matter,
// only that each class gets its own stream.  DBs in one process that share a
// filesystem share its streams; the streams of a filesystem are released
// when the last DB using them is closed.  Allocation needs CAP_SYS_ADMIN.
//
// Allocation failures and failures to attach a file are reported in the info
// log, and DB::Close() logs per-filesystem counts.  The counts, and the
// warnings for files on filesystems without streams, are shared by all DBs
// in the process that use the filesystem, not kept per DB.  The DB directories
// must not be on an XFS realtime volume: streams are allocated on the data
// device, and attaching them to realtime files fails.

#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "rocksdb/env.h"

namespace ROCKSDB_NAMESPACE {

class Logger;

// Called from DB::Open() and DB::CreateColumnFamily().  Takes a reference on
// the streams of the filesystem of each of @dirs, allocating them on first
// use, unless that filesystem is already in @devs; adds the filesystems
// newly referenced to @devs, to be passed to WriteStreamsRelease().
void WriteStreamsAcquire(const std::vector<std::string>& dirs,
                         const std::shared_ptr<Logger>& info_log,
                         std::vector<uint64_t>* devs);

// Called from DB::Close().  Logs what was placed on each filesystem in
// @devs, and drops the references taken by WriteStreamsAcquire().
void WriteStreamsRelease(const std::vector<uint64_t>& devs,
                         const std::shared_ptr<Logger>& info_log);

// Attaches the stream for @hint to the open file @fd, or detaches the file
// if @hint has no stream.  Returns false, after logging, if that failed; the
// file then keeps its previous placement.
bool WriteStreamsSet(int fd, Env::WriteLifeTimeHint hint);

}  // namespace ROCKSDB_NAMESPACE
