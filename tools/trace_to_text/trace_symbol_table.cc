/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/trace_to_text/trace_symbol_table.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/profiling/symbolizer.h"
#include "perfetto/protozero/proto_utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/utils.h"

#include "tools/trace_to_text/utils.h"

#include "protos/perfetto/trace/profiling/profile_common.pb.h"
#include "protos/perfetto/trace/profiling/profile_packet.pb.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pb.h"

#include "protos/perfetto/trace/interned_data/interned_data.pb.h"

namespace perfetto {
namespace trace_to_text {
namespace {

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

using ::perfetto::protos::Callstack;
using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedData;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfiledFrameSymbols;
using ::perfetto::protos::ProfilePacket;

}  // namespace

bool TraceSymbolTable::AddInternedString(const InternedString& string) {
  interned_strings_.emplace(string.iid(), string.str());
  max_string_intern_id_ =
      std::max<uint64_t>(string.iid(), max_string_intern_id_);
  return true;
}

bool TraceSymbolTable::AddMapping(const Mapping& mapping) {
  mappings_.emplace(mapping.iid(), ResolveMapping(mapping));
  return true;
}

bool TraceSymbolTable::AddFrame(const Frame& frame) {
  if (symbols_for_frame_.find(frame.iid()) == symbols_for_frame_.end()) {
    to_symbolize_[frame.mapping_id()].emplace_back(frame.iid());
    rel_pc_for_frame_[frame.iid()] = frame.rel_pc();
  }
  return true;
}

bool TraceSymbolTable::AddProfiledFrameSymbols(
    const protos::ProfiledFrameSymbols& symbol) {
  std::vector<SymbolizedFrame> frames;
  const auto& name_ids = symbol.function_name_id();
  const auto& file_ids = symbol.file_name_id();
  const auto& lines = symbol.line_number();

  if (name_ids.size() != file_ids.size() || file_ids.size() != lines.size()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid ProfiledFrameSymbols");
    return false;
  }

  for (int i = 0; i < name_ids.size(); ++i) {
    SymbolizedFrame frame{ResolveString(name_ids.Get(i)),
                          ResolveString(file_ids.Get(i)), lines.Get(i)};
    frames.emplace_back(std::move(frame));
  }

  symbols_for_frame_[symbol.frame_iid()] = std::move(frames);
  return true;
}

bool TraceSymbolTable::Finalize() {
  if (symbolizer_ == nullptr)
    return true;

  for (const auto& mapping_and_frame_iids : to_symbolize_) {
    auto it = mappings_.find(mapping_and_frame_iids.first);
    if (it == mappings_.end()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid mapping.");
      return false;
    }
    const ResolvedMapping& mapping = it->second;
    const std::vector<uint64_t>& frame_iids = mapping_and_frame_iids.second;
    std::vector<uint64_t> rel_pcs;
    for (uint64_t frame_iid : frame_iids)
      rel_pcs.emplace_back(rel_pc_for_frame_[frame_iid]);
    auto result =
        symbolizer_->Symbolize(mapping.mapping_name, mapping.build_id, rel_pcs);
    if (result.empty())
      continue;
    if (result.size() != frame_iids.size()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid response from symbolizer.");
      return false;
    }
    for (size_t i = 0; i < frame_iids.size(); ++i) {
      if (!result.empty())
        symbols_for_frame_[frame_iids[i]] = std::move(result[i]);
    }
  }
  return true;
}

const std::vector<SymbolizedFrame>* TraceSymbolTable::Get(
    uint64_t frame_iid) const {
  auto it = symbols_for_frame_.find(frame_iid);
  if (it == symbols_for_frame_.end())
    return nullptr;
  return &it->second;
}

const std::string& TraceSymbolTable::ResolveString(uint64_t iid) {
  auto it = interned_strings_.find(iid);
  if (it == interned_strings_.end())
    return kEmptyString;
  return it->second;
}

TraceSymbolTable::ResolvedMapping TraceSymbolTable::ResolveMapping(
    const Mapping& mapping) {
  std::string path;
  for (uint64_t iid : mapping.path_string_ids()) {
    path += "/";
    path += ResolveString(iid);
  }
  return {std::move(path), ResolveString(mapping.build_id())};
}

}  // namespace trace_to_text
}  // namespace perfetto
