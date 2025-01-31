#ifndef TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_MULTILEVEL_EMBEDDING_H_
#define TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_MULTILEVEL_EMBEDDING_H_

#include "tensorflow/core/framework/embedding/cache.h"
#include "tensorflow/core/framework/embedding/config.pb.h"
#include "tensorflow/core/framework/embedding/dense_hash_map.h"
#include "tensorflow/core/framework/embedding/leveldb_kv.h"
#include "tensorflow/core/framework/embedding/lockless_hash_map.h"
#include "tensorflow/core/framework/embedding/kv_interface.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/core/status.h"

namespace tensorflow {
template <class V>
class ValuePtr;
template <class K, class V>
class EmbeddingVar;

namespace embedding {

struct StorageConfig {
  StorageConfig() : type(StorageType::INVALID), path(""), size(0) {}
  StorageConfig(StorageType t,
                const std::string& p,
                int64 s) : type(t), path(p), size(s) {}
  StorageType type;
  std::string path;
  int64 size;
};

template <class K, class V>
class StorageManager {
 public:
  StorageManager(const string& name,
                 StorageConfig sc,
                 size_t cap = -1)
: hash_table_count_(0),
  name_(name),
  sc_(sc),
  cache_(nullptr),
  cache_capacity_(cap),
  eviction_thread_(nullptr),
  total_dims_(0),
  alloc_len_(0) {}

  ~StorageManager() {
    if (eviction_thread_) {
      mutex_lock l(mu_);
      shutdown_cv_.notify_all();
      shutdown_ = true;
    }
    delete eviction_thread_;
    for (auto kv: kvs_) {
      delete kv.first;
    }
    delete cache_;
  }

  Status Init() {
    new_value_ptr_fn_ = [] (Allocator* alloc, size_t size) { return new NormalContiguousValuePtr<V>(alloc, size); };
    switch (sc_.type) {
      case StorageType::DRAM:
        VLOG(1) << "StorageManager::DRAM: " << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(), ev_allocator()));
        break;
      case StorageType::PMEM_MEMKIND:
        VLOG(1) << "StorageManager::PMEM_MEMKIND: " << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(), pmem_allocator()));
        break;
      case StorageType::PMEM_LIBPMEM:
        VLOG(1) << "StorageManager::PMEM_LIBPMEM: " << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(),
                                      experimental_pmem_allocator(sc_.path, sc_.size)));
        break;
      case StorageType::LEVELDB:
        VLOG(1) << "StorageManager::LEVELDB: " << name_;
        kvs_.push_back(std::make_pair(new LevelDBKV<K, V>(sc_.path), ev_allocator()));
        break;
      case StorageType::DRAM_PMEM:
        VLOG(1) << "StorageManager::DRAM_PMEM: " << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(), ev_allocator()));
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(),
                                      experimental_pmem_allocator(sc_.path, sc_.size)));
        break;
      case StorageType::DRAM_LEVELDB:
        VLOG(1) << "StorageManager::DRAM_LEVELDB: " << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(), ev_allocator()));
        kvs_.push_back(std::make_pair(new LevelDBKV<K, V>(sc_.path), ev_allocator()));
        break;
      default:
        VLOG(1) << "StorageManager::default" << name_;
        kvs_.push_back(std::make_pair(new LocklessHashMap<K, V>(), ev_allocator()));
        break;
    }
    hash_table_count_ = kvs_.size();
    if (hash_table_count_ > 1) {
      cache_ = new LRUCache<K>();
      eviction_thread_ = Env::Default()->StartThread(ThreadOptions(), "EV_Eviction",
                                                     [this]() { BatchEviction(); });
      thread_pool_.reset(new thread::ThreadPool(Env::Default(), ThreadOptions(),
                                               "MultiLevel_Embedding_Cache", 2,
                                               /*low_latency_hint=*/false));
    }
    // DebugString();
    CHECK(2 >= hash_table_count_) << "Not support multi-level(>2) embedding.";

    return Status::OK();
  }

  void SetAllocLen(int64 value_len, int slot_num){
    while (flag_.test_and_set(std::memory_order_acquire));
    //The start address of every slot should be aligned to 16 bytes, otherwise a coredump will happen in the ApplyOp.
    alloc_len_ = (value_len * sizeof(V) % 16 == 0) ? value_len : value_len + (16 - (sizeof(V) * value_len) % 16) / sizeof(V);
    int64 temp = alloc_len_ * slot_num;
    if (total_dims_ == 0) {
      total_dims_ = temp;
      if (sc_.type == StorageType::LEVELDB) {
        kvs_[0].first->SetTotalDims(total_dims_);
      } else if (sc_.type == StorageType::DRAM_LEVELDB) {
        kvs_[1].first->SetTotalDims(total_dims_);
      }
      if (hash_table_count_ > 1) {
        cache_capacity_ = 1024 * 1024 * 1024 / total_dims_ * sizeof(V); 
        done_ = true;
        LOG(INFO) << "Cache cache_capacity: " << cache_capacity_;
      }
    }
    flag_.clear(std::memory_order_release);
  }

  int64 GetAllocLen(){
    return alloc_len_;
  }

  int64 GetOffset(int64 index) {
    return alloc_len_ * index;
  }

  int64 GetTotalDims() {
    return total_dims_;
  }

  void DebugString() {
    LOG(INFO) << "Level Number: " << hash_table_count_;
    LOG(INFO) << "Storage Type: " << sc_.type;
    LOG(INFO) << "Storage Path: " << sc_.path;
  }

  void Schedule(std::function<void()> fn) {
    if (hash_table_count_ > 1) {
      thread_pool_->Schedule(std::move(fn));
    }
  }

  Status GetOrCreate(K key, ValuePtr<V>** value_ptr, size_t size) {
    bool found = false;
    int level = 0;
    for (; level < hash_table_count_; ++level) {
      Status s = kvs_[level].first->Lookup(key, value_ptr);
      if (s.ok()) {
        found = true;
        break;
      }
    }
    if (!found) {
      *value_ptr = new_value_ptr_fn_(kvs_[0].second, size);
    }
    if (level || !found) {
      Status s = kvs_[0].first->Insert(key, *value_ptr);
      if (s.ok()) {
        // Insert Success
        return s;
      } else {
        // Insert Failed, key already exist
        (*value_ptr)->Destroy(kvs_[0].second);
        delete *value_ptr;
        s = kvs_[0].first->Lookup(key, value_ptr);
        return s;
      }
    }
    return Status::OK();
  }

  Status Remove(K key) {
    for (auto kv : kvs_) {
      kv.first->Remove(key);
    }
    return Status::OK();
  }

  int64 Size() const {
    int64 total_size = 0;
    for (auto kv : kvs_) {
      total_size += kv.first->Size();
    }
    return total_size;
  }

  Status GetSnapshot(std::vector<K>* key_list,
                     std::vector<ValuePtr<V>* >* value_ptr_list) {
    for (auto kv : kvs_) {
      TF_CHECK_OK(kv.first->GetSnapshot(key_list, value_ptr_list));
    }
    return Status::OK();
  }

  int64 GetSnapshot(std::vector<K>* key_list, std::vector<V* >* value_list,
                    std::vector<int64>* version_list, std::vector<int64>* freq_list,
                    const EmbeddingConfig& emb_config, EmbeddingFilter<K, V, EmbeddingVar<K, V>>* filter) {
    mutex_lock l(mu_);
    for (auto kv : kvs_) {
      std::vector<ValuePtr<V>* > value_ptr_list;
      std::vector<K> key_list_tmp;
      TF_CHECK_OK(kv.first->GetSnapshot(&key_list_tmp, &value_ptr_list));
      for (int64 i = 0; i < key_list_tmp.size(); ++i) {
        V* val = value_ptr_list[i]->GetValue(emb_config.emb_index, GetOffset(emb_config.emb_index));
        V* primary_val = value_ptr_list[i]->GetValue(emb_config.primary_emb_index, GetOffset(emb_config.primary_emb_index));
        if (val != nullptr && primary_val != nullptr) {
          value_list->push_back(val);
          key_list->push_back(key_list_tmp[i]);
          if (emb_config.filter_freq != 0 || emb_config.is_multi_level) {
            int64 dump_freq = filter->GetFreq(key_list_tmp[i], value_ptr_list[i]);
            freq_list->push_back(dump_freq);
          }
          if (emb_config.steps_to_live != 0) {
            int64 dump_version = value_ptr_list[i]->GetStep();
            version_list->push_back(dump_version);
          }
        }
        // storage_manager_->FreeValuePtr(value_ptr_list[i]);
      }
    }
    return key_list->size();
  }

  Status Shrink(const EmbeddingConfig& emb_config, int64 value_len) {
    mutex_lock l(mu_);
    for (auto kv : kvs_) {
      std::vector<K> key_list;
      std::vector<ValuePtr<V>* > value_ptr_list;
      TF_CHECK_OK(kv.first->GetSnapshot(&key_list, &value_ptr_list));
      std::vector<std::pair<K, ValuePtr<V>* > > to_deleted;
      for (int64 i = 0; i < key_list.size(); ++i) {
        V* val = value_ptr_list[i]->GetValue(emb_config.primary_emb_index, GetOffset(emb_config.primary_emb_index));;
        if (val != nullptr) {
          V l2_weight = 0.0;
          for (int64 j = 0; j < value_len; j++) {
              l2_weight += val[j] * val[j];
          }
          l2_weight *= 0.5;
          if (l2_weight < emb_config.l2_weight_threshold) {
            to_deleted.push_back(std::pair<K, ValuePtr<V>*>(key_list[i], value_ptr_list[i]));
          }
        }
      }
      for (const auto it : to_deleted) {
        // TODO memory recycle
        (it.second)->Destroy(kv.second);
        delete it.second;
        kv.first->Remove(it.first);
      }
    }
    return Status::OK();
  }

  Status Shrink(int64 gs, int64 steps_to_live) {
    mutex_lock l(mu_);
    for (auto kv : kvs_) {
      std::vector<K> key_list;
      std::vector<ValuePtr<V>* > value_ptr_list;
      TF_CHECK_OK(kv.first->GetSnapshot(&key_list, &value_ptr_list));
      std::vector<std::pair<K, ValuePtr<V>* > > to_deleted;
      for (int64 i = 0; i < key_list.size(); ++i) {
        int64 version = value_ptr_list[i]->GetStep();
        if (version == -1) {
          value_ptr_list[i]->SetStep(gs);
        } else {
          if (gs - version > steps_to_live) {
            to_deleted.emplace_back(std::pair<K, ValuePtr<V>*>(key_list[i], value_ptr_list[i]));
          }
        }
      }
      for (const auto it : to_deleted) {
        // TODO memory recycle
        (it.second)->Destroy(kv.second);
        delete it.second;
        kv.first->Remove(it.first);
      }
    }
    return Status::OK();
  }

  Status Destroy() {
    mutex_lock l(mu_);
    std::vector<K> key_list;
    std::vector<ValuePtr<V>* > value_ptr_list;
    kvs_[0].first->GetSnapshot(&key_list, &value_ptr_list);
    for (auto value_ptr : value_ptr_list) {
      value_ptr->Destroy(kvs_[0].second);
      delete value_ptr;
    }
    return Status::OK();
  }

  Status BatchCommit(std::vector<K> keys, std::vector<ValuePtr<V>*> value_ptrs) {
    for (auto kv : kvs_) {
      TF_CHECK_OK(kv.first->BatchCommit(keys, value_ptrs));
    }
    return Status::OK();
  }

  BatchCache<K>* Cache() {
    return cache_;
  }

  Status Commit(K key, const ValuePtr<V>* value_ptr) {
    TF_CHECK_OK(kvs_[0].first->Commit(key, value_ptr));
    return Status::OK();
  }

  void FreeValuePtr(ValuePtr<V>* value_ptr) {
    for (auto kv : kvs_) {
      kv.first->FreeValuePtr(value_ptr);
    }
  }

 private:
  void BatchEviction() {
    Env* env = Env::Default();
    const int kSize = 1000;
    if (cache_capacity_ == -1) {
      while (true) {
        mutex_lock l(mu_);
        if (done_) {
          break;
        }
      }
    }
    K evic_ids[kSize];
    while (true) {
      mutex_lock l(mu_);
      if (shutdown_) {
        break;
      }
      const int kTimeoutMilliseconds = 10 * 1;
      WaitForMilliseconds(&l, &shutdown_cv_, kTimeoutMilliseconds);

      int cache_count = cache_->size();
      if (cache_count > cache_capacity_) {
        // eviction
        int k_size = cache_count - cache_capacity_;
        k_size = std::min(k_size, kSize);
        size_t true_size = cache_->get_evic_ids(evic_ids, k_size);
        ValuePtr<V>* value_ptr;
        for (int64 i = 0; i < true_size; ++i) {
          if (kvs_[0].first->Lookup(evic_ids[i], &value_ptr).ok()) {
            TF_CHECK_OK(kvs_[0].first->Remove(evic_ids[i]));
            TF_CHECK_OK(kvs_[1].first->Commit(evic_ids[i], value_ptr));
            // delete value_ptr is nessary;
          } else {
            // bypass
          }
        }
      }
    }
  }

 private:
  int32 hash_table_count_;
  std::string name_;
  std::vector<std::pair<KVInterface<K, V>*, Allocator*>> kvs_;
  std::function<ValuePtr<V>*(Allocator*, size_t)> new_value_ptr_fn_;
  StorageConfig sc_;

  int64 alloc_len_;
  int64 total_dims_;

  std::unique_ptr<thread::ThreadPool> thread_pool_;
  Thread* eviction_thread_;
  BatchCache<K>* cache_;
  size_t cache_capacity_;
  mutex mu_;
  condition_variable shutdown_cv_;
  bool shutdown_ GUARDED_BY(mu_) = false;

  bool done_ = false;
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;

};

} // embedding
} // tensorflow

#endif // TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_MULTILEVEL_EMBEDDING_H_
