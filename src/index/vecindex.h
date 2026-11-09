#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace mvd {

// Squared Euclidean distance over two float32 arrays.
float l2Squared(const float* a, const float* b, size_t dim);

struct VecHit {
  int64_t id;
  float dist;  // squared L2, matching L2_DISTANCE's contract
};

// In-memory IVF index over int64 keys (the primary key).
//
// The index is only consulted for unfiltered TopK. A predicate on a scalar
// column already forces a visit to every record, so routing a filtered query
// through the index would save no work while costing recall; filtered queries
// scan and evaluate the predicate first.
class VectorIndex {
 public:
  explicit VectorIndex(uint32_t dim = 0) : dim_(dim) {}

  uint32_t dim() const { return dim_; }
  size_t count() const { return count_; }
  bool built() const { return built_; }

  void reset(uint32_t dim);
  void build(const std::vector<int64_t>& ids, const std::vector<float>& vecs);
  void insert(int64_t id, const float* v);
  bool remove(int64_t id);
  bool contains(int64_t id) const { return pos_.find(id) != pos_.end(); }

  // Returns up to k nearest neighbours, ascending distance, ties by id.
  void search(const float* q, size_t k, std::vector<VecHit>& out, int nprobe) const;

  // Brute-force over everything held by the index; used when the index is
  // small enough that probing cannot pay for itself.
  void searchAll(const float* q, size_t k, std::vector<VecHit>& out) const;

  // Re-trains the clustering over the current contents. The caller runs this
  // at a commit boundary, never inside a timed query.
  void rebuildIfNeeded();
  bool needsRebuild() const { return dirty_rebuild_; }

  void setNlist(int n) { nlist_ = n; }
  int nlist() const { return nlist_; }

  std::vector<uint8_t> serialize() const;
  bool deserialize(const uint8_t* data, size_t n, uint32_t expected_dim);

 private:
  struct Cluster {
    std::vector<int64_t> ids;
    std::vector<float> vecs;  // ids.size() * dim, contiguous
  };
  struct Pos {
    uint32_t cluster = 0;
    uint32_t index = 0;
  };

  float assign(const float* v, int* cluster_out) const;

  uint32_t dim_ = 0;
  int nlist_ = 0;         // number of clusters
  int target_nlist_ = 0;  // 0 = auto
  size_t count_ = 0;
  size_t inserts_since_build_ = 0;
  bool built_ = false;
  bool dirty_rebuild_ = false;

  std::vector<float> centroids_;             // nlist * dim
  std::vector<Cluster> clusters_;
  std::unordered_map<int64_t, Pos> pos_;
};

}  // namespace mvd
