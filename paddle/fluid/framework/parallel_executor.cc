/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/parallel_executor.h"
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>
#include "paddle/fluid/framework/ir/graph_helper.h"

#include "paddle/fluid/framework/ir/graph.h"

#include "paddle/fluid/framework/details/fast_threaded_ssa_graph_executor.h"
#include "paddle/fluid/framework/details/multi_devices_helper.h"
#include "paddle/fluid/framework/details/parallel_ssa_graph_executor.h"
#include "paddle/fluid/framework/details/reference_count_pass_helper.h"
#include "paddle/fluid/framework/details/scope_buffered_ssa_graph_executor.h"
#include "paddle/fluid/framework/details/threaded_ssa_graph_executor.h"
#include "paddle/fluid/platform/profiler.h"

#ifdef WITH_GPERFTOOLS
#include "gperftools/profiler.h"
#endif
DEFINE_string(pe_profile_fname, "",
              "Profiler filename for PE, which generated by gperftools."
              "Only valid when compiled `WITH_PRIFILER=ON`. Empty if disable.");
DEFINE_bool(enable_parallel_graph, false,
            "Force disable parallel graph execution mode if set false.");

namespace paddle {
namespace framework {

static std::once_flag gProfileOnce;
#ifdef WITH_GPERFTOOLS
static bool gProfileStarted = false;
#endif
class ParallelExecutorPrivate {
 public:
  explicit ParallelExecutorPrivate(const std::vector<platform::Place> &places)
      : places_(places) {
    if (!FLAGS_pe_profile_fname.empty()) {
      std::call_once(gProfileOnce, [] {
#ifdef WITH_GPERFTOOLS
        ProfilerStart(FLAGS_pe_profile_fname.c_str());
        gProfileStarted = true;
#else
        LOG(WARNING) << "Paddle is not compiled with gperftools. "
                        "FLAGS_pe_profile_fname will be ignored";
#endif
      });
    }
  }

  ~ParallelExecutorPrivate() {
    if (own_local_scope_) {
      for (size_t i = 1; i < local_scopes_.size(); ++i) {
        // Skip the first scope, since it is the global scope.
        Scope *local_scope = local_scopes_[i];
        if (global_scope_->HasKid(local_scope)) {
          global_scope_->DeleteScope(local_scope);
        }
      }
    }
  }

  std::unique_ptr<ir::Graph> PrepareGCAndRefCnts(
      std::unique_ptr<ir::Graph> graph, size_t max_memory_size);

  inline bool HasGarbageCollectors() const { return !gcs_.empty(); }

  void ResetRuntimeReferenceCount(const std::vector<std::string> &fetch_tensors,
                                  const std::string &fetched_var_name) {
    for (size_t i = 0; i < runtime_ref_cnts_.size(); ++i) {
      for (auto &pair : global_ref_cnts_[i]) {
        runtime_ref_cnts_[i][pair.first] = pair.second;
      }

      for (auto &fetch_name : fetch_tensors) {
        runtime_ref_cnts_[i].erase(fetch_name);
      }
      runtime_ref_cnts_[i].erase(fetched_var_name);
    }
  }

  BuildStrategy build_strategy_;
  std::vector<platform::Place> places_;
  std::vector<Scope *> local_scopes_;
  Scope *global_scope_;  // not owned
  std::unique_ptr<details::SSAGraphExecutor> executor_;

#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
  std::unique_ptr<platform::NCCLContextMap> nccl_ctxs_;
#endif
  bool own_local_scope_;
  bool use_cuda_;
  bool use_all_reduce_;
  size_t nranks_;

  // global_ref_cnts_ is only initialized when ParallelExecutor constructs, and
  // then keeps unchanged
  // Before each iteration, runtime_ref_cnts_ is reset to global_ref_cnts_
  std::vector<details::ReferenceCountMap> global_ref_cnts_;
  std::vector<details::AtomicReferenceCountMap> runtime_ref_cnts_;
  details::GarbageCollectorMap gcs_;
};

std::unique_ptr<ir::Graph> ParallelExecutorPrivate::PrepareGCAndRefCnts(
    std::unique_ptr<ir::Graph> graph, size_t max_memory_size) {
  for (size_t i = 0; i < places_.size(); ++i) {
    auto &place = places_[i];
    if (gcs_.count(place) > 0) {
      continue;
    }
    std::unique_ptr<GarbageCollector> gc;
#ifdef PADDLE_WITH_CUDA
    if (platform::is_gpu_place(place)) {
      if (IsFastEagerDeletionModeEnabled()) {
        gc.reset(new UnsafeFastGPUGarbageCollector(
            boost::get<platform::CUDAPlace>(place), max_memory_size));
      } else {
        gc.reset(new StreamGarbageCollector(
            boost::get<platform::CUDAPlace>(place), max_memory_size));
      }
      VLOG(10) << "Created " << i << "-th GarbageCollector at " << place;
    } else {
#endif
      if (platform::is_cpu_place(place)) {
        gc.reset(new CPUGarbageCollector(boost::get<platform::CPUPlace>(place),
                                         max_memory_size));
        VLOG(10) << "Created GarbageCollector at " << place;
      } else {
        PADDLE_THROW("Unsupported place for garbage collection");
      }
#ifdef PADDLE_WITH_CUDA
    }
#endif

    gcs_.emplace(place, std::move(gc));
  }

  if (!gcs_.empty()) {
    std::vector<details::LastLiveOpsOfVars> last_live_ops_of_vars;

    auto ref_cnt_pass =
        ir::PassRegistry::Instance().Get("reference_count_pass");
    ref_cnt_pass->SetNotOwned(details::kGlobalReferenceCount,
                              &global_ref_cnts_);
    ref_cnt_pass->SetNotOwned(details::kLastLiveOpsOfVars,
                              &last_live_ops_of_vars);
    graph = ref_cnt_pass->Apply(std::move(graph));
    VLOG(10) << "ReferenceCountPass Applied";

    auto eager_deletion_pass =
        ir::PassRegistry::Instance().Get("eager_deletion_pass");
    eager_deletion_pass->SetNotOwned(details::kRuntimeReferenceCount,
                                     &runtime_ref_cnts_);
    eager_deletion_pass->SetNotOwned(details::kGarbageCollector, &gcs_);
    eager_deletion_pass->SetNotOwned(details::kLastLiveOpsOfVars,
                                     &last_live_ops_of_vars);
    eager_deletion_pass->SetNotOwned(details::kAllPlaces, &places_);
    graph = eager_deletion_pass->Apply(std::move(graph));
    VLOG(10) << "EagerDeletionPass Applied";

    if (build_strategy_.memory_early_delete_) {
      auto early_delete_pass =
          ir::PassRegistry::Instance().Get("memory_early_delete_pass");
      early_delete_pass->SetNotOwned(details::kGarbageCollector, &gcs_);
      graph = early_delete_pass->Apply(std::move(graph));
    }
    VLOG(10) << "MemoryEarlyDeletePass Applied.";
  }

  return graph;
}

std::vector<Scope *> &ParallelExecutor::GetLocalScopes() {
  return member_->local_scopes_;
}

ParallelExecutor::ParallelExecutor(
    const std::vector<platform::Place> &places,
    const std::unordered_set<std::string> &bcast_vars,
    const ProgramDesc &main_program, const std::string &loss_var_name,
    Scope *scope, const std::vector<Scope *> &local_scopes,
    const ExecutionStrategy &exec_strategy, const BuildStrategy &build_strategy)
    : member_(new ParallelExecutorPrivate(places)) {
  member_->global_scope_ = scope;
  member_->use_cuda_ = exec_strategy.use_cuda_;
  member_->build_strategy_ = build_strategy;
  member_->use_all_reduce_ =
      build_strategy.reduce_ == BuildStrategy::ReduceStrategy::kAllReduce;
  member_->nranks_ = num_trainers * places.size();

  if (!member_->use_all_reduce_) {
    PADDLE_ENFORCE(places.size() > 1,
                   "If you set build_strategy.reduce with 'Reduce',"
                   "the number of places must be greater than 1.");
  }

  // Step 1. Bcast the bcast_vars to devs.
  // Create local scopes
  if (local_scopes.empty()) {
    member_->own_local_scope_ = true;
    member_->local_scopes_.emplace_back(member_->global_scope_);
    for (size_t i = 1; i < member_->places_.size(); ++i) {
      member_->local_scopes_.emplace_back(&scope->NewScope());
    }
  } else {
    member_->own_local_scope_ = false;
    PADDLE_ENFORCE_EQ(member_->places_.size(), local_scopes.size());
    for (size_t i = 0; i < member_->places_.size(); ++i) {
      member_->local_scopes_.emplace_back(&local_scopes[i]->NewScope());
    }
  }

  // FIXME(Yancey1989): parallel graph mode get better performance
  // in GPU allreduce distributed training. Need an elegant way to
  // choice the execution strategy.
  build_strategy.enable_parallel_graph_ =
      EnableParallelGraphExecution(main_program, exec_strategy, build_strategy);

  VLOG(1) << "Enable ParallelGraph Execution: "
          << build_strategy.enable_parallel_graph_;

  if (member_->use_cuda_) {
// Bcast Parameters to all GPUs
#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
    ncclUniqueId *nccl_id = nullptr;
    // gen_nccl_id operator can broadcast the ncclUniqueId for nccl2 collective
    // distributed training
    auto *nccl_id_var = scope->FindVar(NCCL_ID_VARNAME);
    if (nccl_id_var != nullptr) {
      nccl_id = nccl_id_var->GetMutable<ncclUniqueId>();
    }
    if (build_strategy.enable_parallel_graph_ && member_->nranks_ > 1UL) {
      if (nccl_id == nullptr) {
        local_nccl_id_.reset(new ncclUniqueId());
        platform::dynload::ncclGetUniqueId(local_nccl_id_.get());
        nccl_id = local_nccl_id_.get();
      }
    }

    member_->nccl_ctxs_.reset(new platform::NCCLContextMap(
        member_->places_, nccl_id, build_strategy.num_trainers_,
        build_strategy.trainer_id_));
#else
    PADDLE_THROW("Not compiled with CUDA");
#endif
  }
  if (member_->local_scopes_.size() != 1 && local_scopes.empty()) {
    BCastParamsToDevices(bcast_vars);
  }
  // Startup Program has been run. All local scopes has correct parameters.

  // Step 2. Convert main_program to SSA form and dependency graph. Also, insert
  // ncclOp
  std::vector<std::unique_ptr<ir::Graph>> graphs;
#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
  if (build_strategy.enable_parallel_graph_) {
    for (size_t i = 0; i < member_->places_.size(); ++i) {
      std::unique_ptr<ir::Graph> graph = build_strategy.Apply(
          main_program, {member_->places_[i]}, loss_var_name,
          {member_->local_scopes_[i]}, member_->nranks_, member_->use_cuda_,
          member_->nccl_ctxs_.get());
      graphs.push_back(std::move(graph));
    }
  } else {
    std::unique_ptr<ir::Graph> graph = build_strategy.Apply(
        main_program, member_->places_, loss_var_name, member_->local_scopes_,
        member_->nranks_, member_->use_cuda_, member_->nccl_ctxs_.get());
    graphs.push_back(std::move(graph));
  }
#else
  std::unique_ptr<ir::Graph> graph = build_strategy.Apply(
      main_program, member_->places_, loss_var_name, member_->local_scopes_,
      member_->nranks_, member_->use_cuda_);
  graphs.push_back(std::move(graph));
#endif
  auto max_memory_size = GetEagerDeletionThreshold();
  if (max_memory_size >= 0) {
    for (size_t i = 0; i < graphs.size(); ++i) {
      graphs[i] = member_->PrepareGCAndRefCnts(
          std::move(graphs[i]), static_cast<size_t>(max_memory_size));
    }
  }

  // Step 3. Create vars in each scope. Passes may also create new vars.
  //         skip control vars and empty vars
  std::vector<details::VariableInfo> var_infos;
  for (auto &graph : graphs) {
    for (auto &node : graph->Nodes()) {
      if (node->IsVar() && !node->IsCtrlVar() && node->Var()) {
        var_infos.emplace_back();
        var_infos.back().name_ = node->Var()->Name();
        var_infos.back().type_ = node->Var()->GetType();
        var_infos.back().persistable_ = node->Var()->Persistable();
      }
    }
  }

  // If the loss_var_name is given, the number of graph should be only one.
  if (loss_var_name.size()) {
    size_t graph_num = ir::GraphNum(*graphs[0]);
    if (graph_num > 1) {
      LOG(WARNING)
          << "The number of graph should be only one, "
             "but the current graph has "
          << ir::GraphNum(*graphs[0])
          << " sub_graphs. If you want to see the nodes of the "
             "sub_graphs, you should use 'FLAGS_print_sub_graph_dir' "
             "to specify the output dir. NOTES: if you not do training, "
             "please don't pass loss_var_name.";
    }
  }

  if (build_strategy.enable_parallel_graph_) {
    member_->executor_.reset(new details::ParallelSSAGraphExecutor(
        exec_strategy, member_->local_scopes_, member_->places_,
        std::move(graphs)));
  } else {
    if (exec_strategy.type_ == ExecutionStrategy::kDefault) {
      member_->executor_.reset(new details::ThreadedSSAGraphExecutor(
          exec_strategy, member_->local_scopes_, member_->places_,
          std::move(graphs[0])));
    } else {
      member_->executor_.reset(new details::FastThreadedSSAGraphExecutor(
          exec_strategy, member_->local_scopes_, member_->places_,
          std::move(graphs[0])));
    }
  }

  member_->executor_.reset(new details::ScopeBufferedSSAGraphExecutor(
      exec_strategy, member_->local_scopes_, std::move(var_infos),
      member_->places_, std::move(member_->executor_)));
}

void ParallelExecutor::BCastParamsToDevices(
    const std::unordered_set<std::string> &vars) const {
  // the initializing bcast, all vars would be bcast from device(0).
  for (auto &var : vars) {
    framework::Variable *main_var = member_->local_scopes_[0]->FindVar(var);
    if (main_var == nullptr || !main_var->IsType<LoDTensor>()) {
      continue;
    }

    auto &main_tensor = main_var->Get<LoDTensor>();
    if (!main_tensor.IsInitialized()) {
      VLOG(3) << "one in var not inited, return!";
      continue;
    }
    auto &dims = main_tensor.dims();
    if (paddle::platform::is_gpu_place(main_tensor.place())) {
#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
      std::vector<void *> buffers;
      buffers.reserve(member_->places_.size());
      size_t numel = main_tensor.numel();
      ncclDataType_t data_type = platform::ToNCCLDataType(main_tensor.type());
      for (size_t i = 0; i < member_->places_.size(); ++i) {
        auto place = member_->places_[i];
        void *buffer;

        if (i == 0) {
          buffer = const_cast<void *>(main_tensor.data<void>());
        } else {
          auto local_scope = member_->local_scopes_[i];
          auto *t = local_scope->Var(var)->GetMutable<LoDTensor>();
          t->Resize(dims);
          buffer = t->mutable_data(place, main_tensor.type());
        }
        buffers.push_back(buffer);
      }

      PADDLE_ENFORCE_EQ(member_->places_.size(), buffers.size(),
                        "variables' buffer size to bcast NOT equal to places");
      {
        platform::NCCLGroupGuard guard;
        for (size_t i = 0; i < member_->places_.size(); ++i) {
          auto &nccl_ctx = member_->nccl_ctxs_->at(member_->places_[i]);
          platform::dynload::ncclBcast(buffers[i], numel, data_type, 0,
                                       nccl_ctx.comm_, nccl_ctx.stream());
        }
        member_->nccl_ctxs_->WaitAll();
      }
#else
      PADDLE_THROW("Not compiled with CUDA");
#endif
    } else {
      platform::CPUPlace cpu;
      for (size_t i = 1; i < member_->places_.size(); ++i) {
        auto local_scope = member_->local_scopes_[i];
        auto *t = local_scope->Var(var)->GetMutable<LoDTensor>();

        // FIXME(zcd): LR_DECAY_COUNTER should not be shared. This is a hot fix.
        if (member_->use_all_reduce_ || member_->use_cuda_ ||
            var == "@LR_DECAY_COUNTER@") {
          t->Resize(dims);
          t->mutable_data(cpu, main_tensor.type());
          paddle::framework::TensorCopy(main_tensor, cpu, t);
        } else {
          t->ShareDataWith(main_tensor);
        }
      }
    }
  }
}

void ParallelExecutor::Run(const std::vector<std::string> &fetch_tensors,
                           const std::string &fetched_var_name) {
#ifdef WITH_GPERFTOOLS
  if (gProfileStarted) {
    ProfilerFlush();
  }
#endif

  platform::RecordBlock b(0);
  if (member_->HasGarbageCollectors()) {
    member_->ResetRuntimeReferenceCount(fetch_tensors, fetched_var_name);
  }
  auto fetch_data = member_->executor_->Run(fetch_tensors);
  *member_->global_scope_->Var(fetched_var_name)->GetMutable<FeedFetchList>() =
      fetch_data;
}

void ParallelExecutor::FeedTensorsIntoLocalScopes(
    const std::vector<std::unordered_map<std::string, LoDTensor>> &tensors) {
  PADDLE_ENFORCE_EQ(member_->local_scopes_.size(), tensors.size());

  for (size_t i = 0; i < tensors.size(); ++i) {
    auto &map = tensors[i];
    auto *scope = member_->local_scopes_[i];
    for (auto &pair : map) {
      auto *trg = scope->Var(pair.first)->GetMutable<LoDTensor>();
      trg->ShareDataWith(pair.second);
      trg->set_lod(pair.second.lod());
    }
  }
}

void ParallelExecutor::FeedAndSplitTensorIntoLocalScopes(
    const std::unordered_map<std::string, LoDTensor> &tensors) {
  for (auto pair : tensors) {
    auto lod_tensors = pair.second.SplitLoDTensor(member_->places_);
    PADDLE_ENFORCE_EQ(
        member_->places_.size(), lod_tensors.size(),
        "The number of samples of current batch is less than the count of "
        "devices, currently, it is not allowed. (%d vs %d)",
        member_->places_.size(), lod_tensors.size());
    for (size_t j = 0; j < member_->places_.size(); ++j) {
      // TODO(panxy0718): Do I need to delete this var?
      auto t =
          member_->local_scopes_[j]->Var(pair.first)->GetMutable<LoDTensor>();
      t->ShareDataWith(lod_tensors[j]);
      t->set_lod(lod_tensors[j].lod());
    }
  }
}

bool ParallelExecutor::EnableParallelGraphExecution(
    const ProgramDesc &main_program, const ExecutionStrategy &exec_strategy,
    const BuildStrategy &build_strategy) const {
  if (!FLAGS_enable_parallel_graph) return false;

  bool enable_parallel_graph = true;
  // TODO(Yancey1989): support sparse update in ParallelGraph mode.
  for (auto &var_desc : main_program.Block(0).AllVars()) {
    if (var_desc->GetType() == proto::VarType::SELECTED_ROWS) {
      enable_parallel_graph = false;
    }
  }

  // TODO(Yancey1989): support pserver mode
  for (auto &op_desc : main_program.Block(0).AllOps()) {
    if (op_desc->Type() == "send" || op_desc->Type() == "recv") {
      enable_parallel_graph = false;
      break;
    }
  }

  if (!member_->use_all_reduce_ || !member_->use_cuda_)
    enable_parallel_graph = false;

  if (build_strategy.enable_sequential_execution_ ||
      exec_strategy.type_ == ExecutionStrategy::ExecutorType::kExperimental)
    enable_parallel_graph = false;
  return enable_parallel_graph;
}

ParallelExecutor::~ParallelExecutor() {
  for (auto &p : member_->places_) {
    platform::DeviceContextPool::Instance().Get(p)->Wait();
  }
  delete member_;
}

}  // namespace framework
}  // namespace paddle

USE_PASS(memory_early_delete_pass);
USE_PASS(reference_count_pass);
USE_PASS(eager_deletion_pass);
