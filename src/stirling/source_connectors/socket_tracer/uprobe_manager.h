/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/synchronization/mutex.h>

#include "src/common/system/proc_parser.h"
#include "src/stirling/bpf_tools/bcc_wrapper.h"
#include "src/stirling/obj_tools/dwarf_reader.h"
#include "src/stirling/obj_tools/elf_reader.h"

#include "src/stirling/source_connectors/socket_tracer/bcc_bpf_intf/socket_trace.hpp"
#include "src/stirling/source_connectors/socket_tracer/bcc_bpf_intf/symaddrs.h"

#include "src/stirling/source_connectors/socket_tracer/uprobe_symaddrs.h"
#include "src/stirling/utils/detect_application.h"
#include "src/stirling/utils/monitor.h"
#include "src/stirling/utils/proc_path_tools.h"
#include "src/stirling/utils/proc_tracker.h"

DECLARE_bool(stirling_rescan_for_dlopen);
DECLARE_double(stirling_rescan_exp_backoff_factor);

namespace px {
namespace stirling {

using px::stirling::RawFptrManager;
using system::ProcParser;

/**
 * Describes a UProbe template.
 * In particular, allows for partial symbol matches using SymbolMatchType.
 */
struct UProbeTmpl {
  std::string_view symbol;
  obj_tools::SymbolMatchType match_type;
  std::string_view probe_fn;
  bpf_tools::BPFProbeAttachType attach_type = bpf_tools::BPFProbeAttachType::kEntry;
};

// A wrapper around BPF maps that are exclusively written by user-space.
// Provides an optimized RemoveValue() interface that avoids the BPF access
// if the key doesn't exist.
template <typename TKeyType, typename TValueType,
          typename TMapType = ebpf::BPFHashTable<TKeyType, TValueType>>
class UserSpaceManagedBPFMap {
 public:
  static std::unique_ptr<UserSpaceManagedBPFMap> Create(bpf_tools::BCCWrapper* bcc,
                                                        const std::string& map_name) {
    return std::unique_ptr<UserSpaceManagedBPFMap>(new UserSpaceManagedBPFMap(bcc, map_name));
  }

  void UpdateValue(TKeyType key, TValueType value) {
    ebpf::StatusTuple s = map_->update_value(key, value);
    if (s.ok()) {
      shadow_keys_.insert(key);
    } else {
      LOG(WARNING) << absl::StrCat("Could not update BPF map. Message=", s.msg());
    }
  }

  void RemoveValue(TKeyType key) {
    if (shadow_keys_.contains(key)) {
      map_->remove_value(key);
      shadow_keys_.erase(key);
    }
  }

 private:
  UserSpaceManagedBPFMap(bpf_tools::BCCWrapper* bcc, const std::string& map_name) {
    if constexpr (std::is_same_v<TMapType, ebpf::BPFMapInMapTable<TKeyType>>) {
      map_ = std::make_unique<TMapType>(bcc->GetMapInMapTable<TKeyType>(map_name));
    } else {
      map_ = std::make_unique<TMapType>(bcc->GetHashTable<TKeyType, TValueType>(map_name));
    }
  }

  std::unique_ptr<TMapType> map_;
  absl::flat_hash_set<TKeyType> shadow_keys_;
};

/**
 * UProbeManager manages the deploying of all uprobes on behalf of the SocketTracer.
 * This includes: OpenSSL uprobes, GoTLS uprobes and Go HTTP2 uprobes.
 */
class UProbeManager {
 public:
  /**
   * Construct a UProbeManager.
   * @param bcc A pointer to a BCCWrapper instance that is used to deploy uprobes.
   */
  explicit UProbeManager(bpf_tools::BCCWrapper* bcc);

  /**
   * Mandatory initialization step before RunDeployUprobesThread can be called.
   * @param enable_http2_tracing Whether to enable HTTP2 tracing.
   * @param disable_self_tracing Whether to enable uprobe deployment on Stirling itself.
   */
  void Init(bool enable_http2_tracing, bool disable_self_tracing = true);

  /**
   * Notify uprobe manager of an mmap event. An mmap may be indicative of a dlopen,
   * so this is used to determine when to rescan binaries for newly loaded shared libraries.
   * @param upid UPID of the process that performed the mmap.
   */
  void NotifyMMapEvent(upid_t upid);

  /**
   * Runs the uprobe deployment code on the provided set of pids, as a thread.
   * @param pids New PIDs to analyze deploy uprobes on. Old PIDs can also be provided,
   *             if they need to be rescanned.
   * @return thread that handles the uprobe deployment work.
   */
  std::thread RunDeployUProbesThread(const absl::flat_hash_set<md::UPID>& pids);

  /**
   * Returns true if a previously dispatched thread (via RunDeployUProbesThread is still running).
   */
  bool ThreadsRunning() { return num_deploy_uprobes_threads_ != 0; }

 private:
  // Probes on Golang crypto/tls library.
  inline static const auto kGoRuntimeUProbeTmpls = MakeArray<UProbeTmpl>({
      UProbeTmpl{
          .symbol = "runtime.casgstatus",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_runtime_casgstatus",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
  });

  inline static constexpr auto kHTTP2ProbeTmpls = MakeArray<UProbeTmpl>({
      // Probes on Golang net/http2 library.
      UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*http2Client).operateHeaders",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_client_operate_headers",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*http2Server).operateHeaders",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_server_operate_headers",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*loopyWriter).writeHeader",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_loopy_writer_write_header",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "golang.org/x/net/http2.(*Framer).WriteDataPadded",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_framer_write_data",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "golang.org/x/net/http2.(*Framer).checkFrameOrder",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_framer_check_frame_order",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },

      // Probes on Golang net/http's implementation of http2.
      UProbeTmpl{
          .symbol = "net/http.(*http2Framer).WriteDataPadded",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2framer_write_data",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "net/http.(*http2Framer).checkFrameOrder",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2framer_check_frame_order",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "net/http.(*http2writeResHeaders).writeFrame",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2writeResHeaders_write_frame",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "golang.org/x/net/http2/hpack.(*Encoder).WriteField",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_hpack_header_encoder",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "net/http.(*http2serverConn).processHeaders",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2serverConn_processHeaders",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
  });

  // Probes on Golang crypto/tls library.
  inline static const auto kGoTLSUProbeTmpls = MakeArray<UProbeTmpl>({
      UProbeTmpl{
          .symbol = "crypto/tls.(*Conn).Write",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_entry_tls_conn_write",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "crypto/tls.(*Conn).Write",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_return_tls_conn_write",
          .attach_type = bpf_tools::BPFProbeAttachType::kReturnInsts,
      },
      UProbeTmpl{
          .symbol = "crypto/tls.(*Conn).Read",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_entry_tls_conn_read",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      UProbeTmpl{
          .symbol = "crypto/tls.(*Conn).Read",
          .match_type = obj_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_return_tls_conn_read",
          .attach_type = bpf_tools::BPFProbeAttachType::kReturnInsts,
      },
  });

  // TODO(yzhao): Regroups OpenSSL uprobes into 3 groups: 1) OpenSSL dynamic library; 2) OpenSSL
  // static library (no known cases other than nodejs today, but should support for future-proof);
  // 3) NodeJS specific uprobes.

  // Probes on node' C++ functions for obtaining the file descriptor from TLSWrap object.
  // The match type is kPrefix to (hopefully) tolerate potential changes in argument
  // order/type/count etc.
  inline static const std::array<UProbeTmpl, 6> kNodeOpenSSLUProbeTmplsV12_3_1 =
      MakeArray<UProbeTmpl>({
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrapC2E",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrapC2E",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrap7ClearInE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrap7ClearInE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrap8ClearOutE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node7TLSWrap8ClearOutE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
      });

  inline static const std::array<UProbeTmpl, 6> kNodeOpenSSLUProbeTmplsV15_0_0 =
      MakeArray<UProbeTmpl>({
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrapC2E",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrapC2E",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrap7ClearInE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrap7ClearInE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrap8ClearOutE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_entry_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          },
          UProbeTmpl{
              .symbol = "_ZN4node6crypto7TLSWrap8ClearOutE",
              .match_type = obj_tools::SymbolMatchType::kPrefix,
              .probe_fn = "probe_ret_TLSWrap_memfn",
              .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          },
      });

  static StatusOr<std::array<UProbeTmpl, 6>> GetNodeOpensslUProbeTmpls(const SemVer& ver);

  // Probes for OpenSSL tracing.
  inline static const auto kOpenSSLUProbes = MakeArray<bpf_tools::UProbeSpec>({
      bpf_tools::UProbeSpec{
          .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
          .symbol = "SSL_write",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          .probe_fn = "probe_entry_SSL_write",
      },
      bpf_tools::UProbeSpec{
          .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
          .symbol = "SSL_write",
          .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          .probe_fn = "probe_ret_SSL_write",
      },
      bpf_tools::UProbeSpec{
          .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
          .symbol = "SSL_read",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
          .probe_fn = "probe_entry_SSL_read",
      },
      bpf_tools::UProbeSpec{
          .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
          .symbol = "SSL_read",
          .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          .probe_fn = "probe_ret_SSL_read",
      },
      // Used by node tracing to record the mapping from SSL object to TLSWrap object.
      // TODO(yzhao): Move this to a separate list for node application only.
      bpf_tools::UProbeSpec{
          .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
          .symbol = "SSL_new",
          .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
          .probe_fn = "probe_ret_SSL_new",
      },
  });

  /**
   * Deploys all available uprobe types (HTTP2, OpenSSL, etc.) on new processes.
   * @param pids The list of pids to analyze and instrument with uprobes, if appropriate.
   */
  void DeployUProbes(const absl::flat_hash_set<md::UPID>& pids);

  /**
   * Deploys all OpenSSL uprobes on new processes.
   * @param pids The list of pids to analyze and instrument with OpenSSL uprobes, if appropriate.
   * @return Number of uprobes deployed.
   */
  int DeployOpenSSLUProbes(const absl::flat_hash_set<md::UPID>& pids);

  /**
   * Deploys all Go uprobes on new processes.
   * @param pids The list of pids to analyze and instrument with Go uprobes, if appropriate.
   * @return Number of uprobes deployed.
   */
  int DeployGoUProbes(const absl::flat_hash_set<md::UPID>& pids);

  /**
   * Sets up the BPF maps used for GOID tracking. Required for general Go tracing.
   *
   * @param binary The path to the binary on which to deploy Go probes.
   * @param pids The list of PIDs that are new instances of the binary.
   */
  void SetupGOIDMaps(const std::string& binary, const std::vector<int32_t>& pids);

  /**
   * Attaches the required probes for general Go tracing to the specified binary, if it is a
   * compatible Go binary.
   *
   * @param binary The path to the binary on which to deploy Go probes.
   * @param elf_reader ELF reader for the binary.
   * @param dwarf_reader DWARF reader for the binary.
   * @param pids The list of PIDs that are new instances of the binary. Used to populate symbol
   *             addresses.
   * @return The number of uprobes deployed, or error. It is not an error if the binary
   *         is not a Go binary; instead the return value will be zero.
   */
  StatusOr<int> AttachGoRuntimeUProbes(const std::string& binary, obj_tools::ElfReader* elf_reader,
                                       obj_tools::DwarfReader* dwarf_reader,
                                       const std::vector<int32_t>& new_pids);

  /**
   * Attaches the required probes for Go HTTP2 tracing to the specified binary, if it is a
   * compatible Go binary.
   *
   * @param binary The path to the binary on which to deploy Go HTTP2 probes.
   * @param elf_reader ELF reader for the binary.
   * @param dwarf_reader DWARF reader for the binary.
   * @param pids The list of PIDs that are new instances of the binary. Used to populate symbol
   *             addresses.
   * @return The number of uprobes deployed, or error. It is not considered an error if the binary
   *         is not a Go binary or doesn't use a Go HTTP2 library; instead the return value will be
   *         zero.
   */
  StatusOr<int> AttachGoHTTP2Probes(const std::string& binary, obj_tools::ElfReader* elf_reader,
                                    obj_tools::DwarfReader* dwarf_reader,
                                    const std::vector<int32_t>& pids);

  /**
   * Attaches the required probes for GoTLS tracing to the specified binary, if it is a compatible
   * Go binary.
   *
   * @param binary The path to the binary on which to deploy Go HTTP2 probes.
   * @param elf_reader ELF reader for the binary.
   * @param dwarf_reader DWARF reader for the binary.
   * @param pids The list of PIDs that are new instances of the binary. Used to populate symbol
   *             addresses.
   * @return The number of uprobes deployed, or error. It is not an error if the binary
   *         is not a Go binary or doesn't use Go TLS; instead the return value will be zero.
   */
  StatusOr<int> AttachGoTLSUProbes(const std::string& binary, obj_tools::ElfReader* elf_reader,
                                   obj_tools::DwarfReader* dwarf_reader,
                                   const std::vector<int32_t>& new_pids);

  /**
   * Attaches the required probes for OpenSSL tracing to the specified PID, if it uses OpenSSL.
   *
   * @param pid The PID of the process whose mount namespace is examined for OpenSSL dynamic library
   * files.
   * @return The number of uprobes deployed. It is not an error if the binary
   *         does not use OpenSSL; instead the return value will be zero.
   */
  StatusOr<int> AttachOpenSSLUProbesOnDynamicLib(uint32_t pid);

  /**
   * Attaches the required probes for OpenSSL tracing the executable of the specified PID.
   * The OpenSSL library is assumed to be statically linked into the executable.
   *
   * @param pid The PID of the process whose executable is attached with the probes.
   * @return The number of uprobes deployed. It is not an error if the binary
   * does not use OpenSSL; instead the return value will be zero.
   */
  StatusOr<int> AttachNodeJsOpenSSLUprobes(uint32_t pid);

  /**
   * Calls BCCWrapper.AttachUProbe() with a probe template and log any errors to the probe status
   * table.
   */
  Status LogAndAttachUProbe(const bpf_tools::UProbeSpec& spec);

  /**
   * Helper function that calls BCCWrapper.AttachUprobe() from a probe template.
   * Among other things, it finds all symbol matches as specified in the template,
   * and attaches a probe per matching symbol.
   *
   * @param probe_tmpls Array of probe templates to process.
   * @param binary The binary to uprobe.
   * @param elf_reader Pointer to an elf reader for the binary. Used to find symbol matches.
   * @return Number of uprobes deployed, or error if uprobes failed to deploy. Zero uprobes
   *         deploying because there are no symbol matches is not considered an error.
   */
  StatusOr<int> AttachUProbeTmpl(const ArrayView<UProbeTmpl>& probe_tmpls,
                                 const std::string& binary, obj_tools::ElfReader* elf_reader);

  // Returns set of PIDs that have had mmap called on them since the last call.
  absl::flat_hash_set<md::UPID> PIDsToRescanForUProbes();

  Status UpdateOpenSSLSymAddrs(RawFptrManager* fptrManager, std::filesystem::path container_lib,
                               uint32_t pid);
  Status UpdateGoCommonSymAddrs(obj_tools::ElfReader* elf_reader,
                                obj_tools::DwarfReader* dwarf_reader,
                                const std::vector<int32_t>& pids);
  Status UpdateGoHTTP2SymAddrs(obj_tools::ElfReader* elf_reader,
                               obj_tools::DwarfReader* dwarf_reader,
                               const std::vector<int32_t>& pids);
  Status UpdateGoTLSSymAddrs(obj_tools::ElfReader* elf_reader, obj_tools::DwarfReader* dwarf_reader,
                             const std::vector<int32_t>& pids);
  Status UpdateNodeTLSWrapSymAddrs(int32_t pid, const std::filesystem::path& node_exe,
                                   const SemVer& ver);

  // Clean-up various BPF maps used to communicate symbol addresses per PID.
  // Once the PID has terminated, the information is not required anymore.
  // Note that BPF maps can fill up if this is not done.
  void CleanupPIDMaps(const absl::flat_hash_set<md::UPID>& deleted_upids);

  bpf_tools::BCCWrapper* bcc_;

  // Whether to try to uprobe ourself (e.g. for OpenSSL). Typically, we don't want to do that.
  bool cfg_disable_self_probing_;

  // Whether we want to enable HTTP2 tracing. When false, we don't deploy HTTP2 uprobes.
  bool cfg_enable_http2_tracing_;

  // Ensures DeployUProbes threads run sequentially.
  std::mutex deploy_uprobes_mutex_;
  std::atomic<int> num_deploy_uprobes_threads_ = 0;

  std::unique_ptr<system::ProcParser> proc_parser_;
  ProcTracker proc_tracker_;
  LazyLoadedFPResolver fp_resolver_;

  absl::flat_hash_set<upid_t> upids_with_mmap_;

  // Count the number of times PIDsToRescanForUProbes() has been called.
  int rescan_counter_ = 0;

  // Map of UPIDs to the periodicity at which they are allowed to be rescanned.
  // The backoff value starts at 1 (meaning they can be scanned every iteration),
  // and exponentially grows every time nothing new is found.
  absl::flat_hash_map<md::UPID, int> backoff_map_;

  // Records the binaries that have uprobes attached, so we don't try to probe them again.
  // TODO(oazizi): How should these sets be cleaned up of old binaries, once they are deleted?
  //               Without clean-up, these could consume more-and-more memory.
  absl::flat_hash_set<std::string> openssl_probed_binaries_;
  absl::flat_hash_set<std::string> scanned_binaries_;
  absl::flat_hash_set<std::string> go_probed_binaries_;
  absl::flat_hash_set<std::string> go_http2_probed_binaries_;
  absl::flat_hash_set<std::string> go_tls_probed_binaries_;
  absl::flat_hash_set<std::string> nodejs_binaries_;

  // BPF maps through which the addresses of symbols for a given pid are communicated to uprobes.
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, struct openssl_symaddrs_t>>
      openssl_symaddrs_map_;
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, struct go_common_symaddrs_t>>
      go_common_symaddrs_map_;
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, struct go_http2_symaddrs_t>>
      go_http2_symaddrs_map_;
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, struct go_tls_symaddrs_t>> go_tls_symaddrs_map_;
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, struct node_tlswrap_symaddrs_t>>
      node_tlswrap_symaddrs_map_;
  std::unique_ptr<UserSpaceManagedBPFMap<uint32_t, int, ebpf::BPFMapInMapTable<uint32_t>>>
      go_goid_map_;

  StirlingMonitor& monitor_ = *StirlingMonitor::GetInstance();
};

}  // namespace stirling
}  // namespace px
