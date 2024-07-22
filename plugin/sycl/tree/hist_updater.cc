/*!
 * Copyright 2017-2024 by Contributors
 * \file hist_updater.cc
 */

#include "hist_updater.h"

#include <oneapi/dpl/random>

#include <functional>

#include "../common/hist_util.h"
#include "../../src/collective/allreduce.h"

namespace xgboost {
namespace sycl {
namespace tree {

using ::sycl::ext::oneapi::plus;
using ::sycl::ext::oneapi::minimum;
using ::sycl::ext::oneapi::maximum;

template <typename GradientSumT>
void HistUpdater<GradientSumT>::SetHistSynchronizer(
    HistSynchronizer<GradientSumT> *sync) {
  hist_synchronizer_.reset(sync);
}

template <typename GradientSumT>
void HistUpdater<GradientSumT>::SetHistRowsAdder(
    HistRowsAdder<GradientSumT> *adder) {
  hist_rows_adder_.reset(adder);
}

template <typename GradientSumT>
void HistUpdater<GradientSumT>::BuildHistogramsLossGuide(
    ExpandEntry entry,
    const common::GHistIndexMatrix &gmat,
    RegTree *p_tree,
    const USMVector<GradientPair, MemoryType::on_device> &gpair_device) {
  nodes_for_explicit_hist_build_.clear();
  nodes_for_subtraction_trick_.clear();
  nodes_for_explicit_hist_build_.push_back(entry);

  if (!(*p_tree)[entry.nid].IsRoot()) {
    auto sibling_id = entry.GetSiblingId(p_tree);
    nodes_for_subtraction_trick_.emplace_back(sibling_id, p_tree->GetDepth(sibling_id));
  }

  std::vector<int> sync_ids;
  hist_rows_adder_->AddHistRows(this, &sync_ids, p_tree);
  qu_.wait_and_throw();
  BuildLocalHistograms(gmat, p_tree, gpair_device);
  hist_synchronizer_->SyncHistograms(this, sync_ids, p_tree);
}

template<typename GradientSumT>
void HistUpdater<GradientSumT>::BuildLocalHistograms(
    const common::GHistIndexMatrix &gmat,
    RegTree *p_tree,
    const USMVector<GradientPair, MemoryType::on_device> &gpair_device) {
  builder_monitor_.Start("BuildLocalHistograms");
  const size_t n_nodes = nodes_for_explicit_hist_build_.size();
  ::sycl::event event;

  for (size_t i = 0; i < n_nodes; i++) {
    const int32_t nid = nodes_for_explicit_hist_build_[i].nid;

    if (row_set_collection_[nid].Size() > 0) {
      event = BuildHist(gpair_device, row_set_collection_[nid], gmat, &(hist_[nid]),
                        &(hist_buffer_.GetDeviceBuffer()), event);
    } else {
      common::InitHist(qu_, &(hist_[nid]), hist_[nid].Size(), &event);
    }
  }
  qu_.wait_and_throw();
  builder_monitor_.Stop("BuildLocalHistograms");
}

template<typename GradientSumT>
void HistUpdater<GradientSumT>::InitSampling(
      const USMVector<GradientPair, MemoryType::on_device> &gpair,
      USMVector<size_t, MemoryType::on_device>* row_indices) {
  const size_t num_rows = row_indices->Size();
  auto* row_idx = row_indices->Data();
  const auto* gpair_ptr = gpair.DataConst();
  uint64_t num_samples = 0;
  const auto subsample = param_.subsample;
  ::sycl::event event;

  {
    ::sycl::buffer<uint64_t, 1> flag_buf(&num_samples, 1);
    uint64_t seed = seed_;
    seed_ += num_rows;
    event = qu_.submit([&](::sycl::handler& cgh) {
      auto flag_buf_acc  = flag_buf.get_access<::sycl::access::mode::read_write>(cgh);
      cgh.parallel_for<>(::sycl::range<1>(::sycl::range<1>(num_rows)),
                                          [=](::sycl::item<1> pid) {
        uint64_t i = pid.get_id(0);

        // Create minstd_rand engine
        oneapi::dpl::minstd_rand engine(seed, i);
        oneapi::dpl::bernoulli_distribution coin_flip(subsample);

        auto rnd = coin_flip(engine);
        if (gpair_ptr[i].GetHess() >= 0.0f && rnd) {
          AtomicRef<uint64_t> num_samples_ref(flag_buf_acc[0]);
          row_idx[num_samples_ref++] = i;
        }
      });
    });
    /* After calling a destructor for flag_buf,  content will be copyed to num_samples */
  }

  row_indices->Resize(&qu_, num_samples, 0, &event);
  qu_.wait();
}

template<typename GradientSumT>
void HistUpdater<GradientSumT>::InitData(
                                const common::GHistIndexMatrix& gmat,
                                const USMVector<GradientPair, MemoryType::on_device> &gpair,
                                const DMatrix& fmat,
                                const RegTree& tree) {
  CHECK((param_.max_depth > 0 || param_.max_leaves > 0))
      << "max_depth or max_leaves cannot be both 0 (unlimited); "
      << "at least one should be a positive quantity.";
  if (param_.grow_policy == xgboost::tree::TrainParam::kDepthWise) {
    CHECK(param_.max_depth > 0) << "max_depth cannot be 0 (unlimited) "
                                << "when grow_policy is depthwise.";
  }
  builder_monitor_.Start("InitData");
  const auto& info = fmat.Info();

  if (!column_sampler_) {
    column_sampler_ = xgboost::common::MakeColumnSampler(ctx_);
  }

  // initialize the row set
  {
    row_set_collection_.Clear();

    // initialize histogram collection
    uint32_t nbins = gmat.cut.Ptrs().back();
    hist_.Init(qu_, nbins);

    hist_buffer_.Init(qu_, nbins);
    size_t buffer_size = kBufferSize;
    hist_buffer_.Reset(kBufferSize);

    // initialize histogram builder
    hist_builder_ = common::GHistBuilder<GradientSumT>(qu_, nbins);

    USMVector<size_t, MemoryType::on_device>* row_indices = &(row_set_collection_.Data());
    row_indices->Resize(&qu_, info.num_row_);
    size_t* p_row_indices = row_indices->Data();
    // mark subsample and build list of member rows
    if (param_.subsample < 1.0f) {
      CHECK_EQ(param_.sampling_method, xgboost::tree::TrainParam::kUniform)
        << "Only uniform sampling is supported, "
        << "gradient-based sampling is only support by GPU Hist.";
      InitSampling(gpair, row_indices);
    } else {
      int has_neg_hess = 0;
      const GradientPair* gpair_ptr = gpair.DataConst();
      ::sycl::event event;
      {
        ::sycl::buffer<int, 1> flag_buf(&has_neg_hess, 1);
        event = qu_.submit([&](::sycl::handler& cgh) {
          auto flag_buf_acc  = flag_buf.get_access<::sycl::access::mode::read_write>(cgh);
          cgh.parallel_for<>(::sycl::range<1>(::sycl::range<1>(info.num_row_)),
                                            [=](::sycl::item<1> pid) {
            const size_t idx = pid.get_id(0);
            p_row_indices[idx] = idx;
            if (gpair_ptr[idx].GetHess() < 0.0f) {
              AtomicRef<int> has_neg_hess_ref(flag_buf_acc[0]);
              has_neg_hess_ref.fetch_max(1);
            }
          });
        });
      }

      if (has_neg_hess) {
        size_t max_idx = 0;
        {
          ::sycl::buffer<size_t, 1> flag_buf(&max_idx, 1);
          event = qu_.submit([&](::sycl::handler& cgh) {
            cgh.depends_on(event);
            auto flag_buf_acc  = flag_buf.get_access<::sycl::access::mode::read_write>(cgh);
            cgh.parallel_for<>(::sycl::range<1>(::sycl::range<1>(info.num_row_)),
                                                [=](::sycl::item<1> pid) {
              const size_t idx = pid.get_id(0);
              if (gpair_ptr[idx].GetHess() >= 0.0f) {
                AtomicRef<size_t> max_idx_ref(flag_buf_acc[0]);
                p_row_indices[max_idx_ref++] = idx;
              }
            });
          });
        }
        row_indices->Resize(&qu_, max_idx, 0, &event);
      }
      qu_.wait_and_throw();
    }
  }
  row_set_collection_.Init();

  {
    /* determine layout of data */
    const size_t nrow = info.num_row_;
    const size_t ncol = info.num_col_;
    const size_t nnz = info.num_nonzero_;
    // number of discrete bins for feature 0
    const uint32_t nbins_f0 = gmat.cut.Ptrs()[1] - gmat.cut.Ptrs()[0];
    if (nrow * ncol == nnz) {
      // dense data with zero-based indexing
      data_layout_ = kDenseDataZeroBased;
    } else if (nbins_f0 == 0 && nrow * (ncol - 1) == nnz) {
      // dense data with one-based indexing
      data_layout_ = kDenseDataOneBased;
    } else {
      // sparse data
      data_layout_ = kSparseData;
    }
  }

  column_sampler_->Init(ctx_, info.num_col_, info.feature_weights.ConstHostVector(),
                        param_.colsample_bynode, param_.colsample_bylevel,
                        param_.colsample_bytree);
  if (data_layout_ == kDenseDataZeroBased || data_layout_ == kDenseDataOneBased) {
    /* specialized code for dense data:
       choose the column that has a least positive number of discrete bins.
       For dense data (with no missing value),
       the sum of gradient histogram is equal to snode[nid] */
    const std::vector<uint32_t>& row_ptr = gmat.cut.Ptrs();
    const auto nfeature = static_cast<bst_uint>(row_ptr.size() - 1);
    uint32_t min_nbins_per_feature = 0;
    for (bst_uint i = 0; i < nfeature; ++i) {
      const uint32_t nbins = row_ptr[i + 1] - row_ptr[i];
      if (nbins > 0) {
        if (min_nbins_per_feature == 0 || min_nbins_per_feature > nbins) {
          min_nbins_per_feature = nbins;
          fid_least_bins_ = i;
        }
      }
    }
    CHECK_GT(min_nbins_per_feature, 0U);
  }

  std::fill(snode_host_.begin(), snode_host_.end(),  NodeEntry<GradientSumT>(param_));
  builder_monitor_.Stop("InitData");
}

template <typename GradientSumT>
void HistUpdater<GradientSumT>::InitNewNode(int nid,
                                            const common::GHistIndexMatrix& gmat,
                                            const USMVector<GradientPair,
                                                            MemoryType::on_device> &gpair,
                                            const DMatrix& fmat,
                                            const RegTree& tree) {
  builder_monitor_.Start("InitNewNode");

  snode_host_.resize(tree.NumNodes(), NodeEntry<GradientSumT>(param_));
  {
    if (tree[nid].IsRoot()) {
      GradStats<GradientSumT> grad_stat;
      if (data_layout_ == kDenseDataZeroBased || data_layout_ == kDenseDataOneBased) {
        const std::vector<uint32_t>& row_ptr = gmat.cut.Ptrs();
        const uint32_t ibegin = row_ptr[fid_least_bins_];
        const uint32_t iend = row_ptr[fid_least_bins_ + 1];
        const auto* hist = reinterpret_cast<GradStats<GradientSumT>*>(hist_[nid].Data());

        std::vector<GradStats<GradientSumT>> ets(iend - ibegin);
        qu_.memcpy(ets.data(), hist + ibegin,
                   (iend - ibegin) * sizeof(GradStats<GradientSumT>)).wait_and_throw();
        for (const auto& et : ets) {
          grad_stat += et;
        }
      } else {
        const common::RowSetCollection::Elem e = row_set_collection_[nid];
        const size_t* row_idxs = e.begin;
        const size_t size = e.Size();
        const GradientPair* gpair_ptr = gpair.DataConst();

        ::sycl::buffer<GradStats<GradientSumT>> buff(&grad_stat, 1);
        qu_.submit([&](::sycl::handler& cgh) {
          auto reduction = ::sycl::reduction(buff, cgh, ::sycl::plus<>());
          cgh.parallel_for<>(::sycl::range<1>(size), reduction,
                            [=](::sycl::item<1> pid, auto& sum) {
            size_t i = pid.get_id(0);
            size_t row_idx = row_idxs[i];
            if constexpr (std::is_same<GradientPair::ValueT, GradientSumT>::value) {
              sum += gpair_ptr[row_idx];
            } else {
              sum += GradStats<GradientSumT>(gpair_ptr[row_idx].GetGrad(),
                                             gpair_ptr[row_idx].GetHess());
            }
          });
        }).wait_and_throw();
      }
      auto rc = collective::Allreduce(
                      ctx_, linalg::MakeVec(reinterpret_cast<GradientSumT*>(&grad_stat), 2),
                      collective::Op::kSum);
      SafeColl(rc);
      snode_host_[nid].stats = grad_stat;
    } else {
      int parent_id = tree[nid].Parent();
      if (tree[nid].IsLeftChild()) {
        snode_host_[nid].stats = snode_host_[parent_id].best.left_sum;
      } else {
        snode_host_[nid].stats = snode_host_[parent_id].best.right_sum;
      }
    }
  }

  // calculating the weights
  {
    auto evaluator = tree_evaluator_.GetEvaluator();
    bst_uint parentid = tree[nid].Parent();
    snode_host_[nid].weight = evaluator.CalcWeight(parentid, snode_host_[nid].stats);
    snode_host_[nid].root_gain = evaluator.CalcGain(parentid, snode_host_[nid].stats);
  }
  builder_monitor_.Stop("InitNewNode");
}

// nodes_set - set of nodes to be processed in parallel
template<typename GradientSumT>
void HistUpdater<GradientSumT>::EvaluateSplits(
                        const std::vector<ExpandEntry>& nodes_set,
                        const common::GHistIndexMatrix& gmat,
                        const RegTree& tree) {
  builder_monitor_.Start("EvaluateSplits");

  const size_t n_nodes_in_set = nodes_set.size();

  using FeatureSetType = std::shared_ptr<HostDeviceVector<bst_feature_t>>;

  // Generate feature set for each tree node
  size_t pos = 0;
  for (size_t nid_in_set = 0; nid_in_set < n_nodes_in_set; ++nid_in_set) {
    const bst_node_t nid = nodes_set[nid_in_set].nid;
    FeatureSetType features_set = column_sampler_->GetFeatureSet(tree.GetDepth(nid));
    for (size_t idx = 0; idx < features_set->Size(); idx++) {
      const size_t fid = features_set->ConstHostVector()[idx];
      if (interaction_constraints_.Query(nid, fid)) {
        auto this_hist = hist_[nid].DataConst();
        if (pos < split_queries_host_.size()) {
          split_queries_host_[pos] = SplitQuery{nid, fid, this_hist};
        } else {
          split_queries_host_.push_back({nid, fid, this_hist});
        }
        ++pos;
      }
    }
  }
  const size_t total_features = pos;

  split_queries_device_.Resize(&qu_, total_features);
  auto event = qu_.memcpy(split_queries_device_.Data(), split_queries_host_.data(),
                          total_features * sizeof(SplitQuery));

  auto evaluator = tree_evaluator_.GetEvaluator();
  SplitQuery* split_queries_device = split_queries_device_.Data();
  const uint32_t* cut_ptr = gmat.cut_device.Ptrs().DataConst();
  const bst_float* cut_val = gmat.cut_device.Values().DataConst();
  const bst_float* cut_minval = gmat.cut_device.MinValues().DataConst();

  snode_device_.ResizeNoCopy(&qu_, snode_host_.size());
  event = qu_.memcpy(snode_device_.Data(), snode_host_.data(),
                     snode_host_.size() * sizeof(NodeEntry<GradientSumT>), event);
  const NodeEntry<GradientSumT>* snode = snode_device_.Data();

  const float min_child_weight = param_.min_child_weight;

  best_splits_device_.ResizeNoCopy(&qu_, total_features);
  if (best_splits_host_.size() < total_features) best_splits_host_.resize(total_features);
  SplitEntry<GradientSumT>* best_splits = best_splits_device_.Data();

  event = qu_.submit([&](::sycl::handler& cgh) {
    cgh.depends_on(event);
    cgh.parallel_for<>(::sycl::nd_range<2>(::sycl::range<2>(total_features, sub_group_size_),
                                           ::sycl::range<2>(1, sub_group_size_)),
                       [=](::sycl::nd_item<2> pid) {
      int i = pid.get_global_id(0);
      auto sg = pid.get_sub_group();
      int nid = split_queries_device[i].nid;
      int fid = split_queries_device[i].fid;
      const GradientPairT* hist_data = split_queries_device[i].hist;

      best_splits[i] = snode[nid].best;
      EnumerateSplit(sg, cut_ptr, cut_val, hist_data, snode[nid],
                     &(best_splits[i]), fid, nid, evaluator, min_child_weight);
    });
  });
  event = qu_.memcpy(best_splits_host_.data(), best_splits,
                     total_features * sizeof(SplitEntry<GradientSumT>), event);

  qu_.wait();
  for (size_t i = 0; i < total_features; i++) {
    int nid = split_queries_host_[i].nid;
    snode_host_[nid].best.Update(best_splits_host_[i]);
  }

  builder_monitor_.Stop("EvaluateSplits");
}

// Enumerate the split values of specific feature.
// Returns the sum of gradients corresponding to the data points that contains a non-missing value
// for the particular feature fid.
template <typename GradientSumT>
void HistUpdater<GradientSumT>::EnumerateSplit(
    const ::sycl::sub_group& sg,
    const uint32_t* cut_ptr,
    const bst_float* cut_val,
    const GradientPairT* hist_data,
    const NodeEntry<GradientSumT>& snode,
    SplitEntry<GradientSumT>* p_best,
    bst_uint fid,
    bst_uint nodeID,
    typename TreeEvaluator<GradientSumT>::SplitEvaluator const &evaluator,
    float min_child_weight) {
  SplitEntry<GradientSumT> best;

  int32_t ibegin = static_cast<int32_t>(cut_ptr[fid]);
  int32_t iend = static_cast<int32_t>(cut_ptr[fid + 1]);

  GradStats<GradientSumT> sum(0, 0);

  int32_t sub_group_size = sg.get_local_range().size();
  const size_t local_id = sg.get_local_id()[0];

  /* TODO(razdoburdin)
   * Currently the first additions are fast and the last are slow.
   * Maybe calculating of reduce overgroup in seprate kernel and reusing it here can be faster
   */
  for (int32_t i = ibegin + local_id; i < iend; i += sub_group_size) {
    sum.Add(::sycl::inclusive_scan_over_group(sg, hist_data[i].GetGrad(), std::plus<>()),
            ::sycl::inclusive_scan_over_group(sg, hist_data[i].GetHess(), std::plus<>()));

    if (sum.GetHess() >= min_child_weight) {
      GradStats<GradientSumT> c = snode.stats - sum;
      if (c.GetHess() >= min_child_weight) {
        bst_float loss_chg = evaluator.CalcSplitGain(nodeID, fid, sum, c) - snode.root_gain;
        bst_float split_pt = cut_val[i];
        best.Update(loss_chg, fid, split_pt, false, sum, c);
      }
    }

    const bool last_iter = i + sub_group_size >= iend;
    if (!last_iter) {
      size_t end = i - local_id + sub_group_size;
      if (end > iend) end = iend;
      for (size_t j = i + 1; j < end; ++j) {
        sum.Add(hist_data[j].GetGrad(), hist_data[j].GetHess());
      }
    }
  }

  bst_float total_loss_chg = ::sycl::reduce_over_group(sg, best.loss_chg, maximum<>());
  bst_feature_t total_split_index = ::sycl::reduce_over_group(sg,
                                                              best.loss_chg == total_loss_chg ?
                                                              best.SplitIndex() :
                                                              (1U << 31) - 1U, minimum<>());
  if (best.loss_chg == total_loss_chg &&
      best.SplitIndex() == total_split_index) p_best->Update(best);
}

template class HistUpdater<float>;
template class HistUpdater<double>;

}  // namespace tree
}  // namespace sycl
}  // namespace xgboost
