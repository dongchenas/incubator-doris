// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/storage_engine.h"

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <queue>
#include <set>
#include <random>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <rapidjson/document.h>
#include <thrift/protocol/TDebugProtocol.h>

#include "olap/base_compaction.h"
#include "olap/cumulative_compaction.h"
#include "olap/lru_cache.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_meta_manager.h"
#include "olap/push_handler.h"
#include "olap/reader.h"
#include "olap/rowset/rowset_meta_manager.h"
#include "olap/rowset/alpha_rowset.h"
#include "olap/rowset_factory.h"
#include "olap/schema_change.h"
#include "olap/data_dir.h"
#include "olap/utils.h"
#include "olap/rowset/alpha_rowset_meta.h"
#include "olap/rowset/column_data_writer.h"
#include "olap/olap_snapshot_converter.h"
#include "util/time.h"
#include "util/doris_metrics.h"
#include "util/pretty_printer.h"

using apache::thrift::ThriftDebugString;
using boost::filesystem::canonical;
using boost::filesystem::directory_iterator;
using boost::filesystem::path;
using boost::filesystem::recursive_directory_iterator;
using std::back_inserter;
using std::copy;
using std::inserter;
using std::list;
using std::map;
using std::nothrow;
using std::pair;
using std::priority_queue;
using std::set;
using std::set_difference;
using std::string;
using std::stringstream;
using std::vector;

namespace doris {

StorageEngine* StorageEngine::_s_instance = nullptr;

static Status _validate_options(const EngineOptions& options) {
    if (options.store_paths.empty()) {
        return Status("store paths is empty");;
    }
    return Status::OK;
}

Status StorageEngine::open(const EngineOptions& options, StorageEngine** engine_ptr) {
    RETURN_IF_ERROR(_validate_options(options));
    std::unique_ptr<StorageEngine> engine(new StorageEngine(options));
    auto st = engine->open();
    if (st != OLAP_SUCCESS) {
        LOG(WARNING) << "engine open failed, res=" << st;
        return Status("open engine failed");
    }
    st = engine->_start_bg_worker();
    if (st != OLAP_SUCCESS) {
        LOG(WARNING) << "engine start background failed, res=" << st;
        return Status("open engine failed");
    }
    *engine_ptr = engine.release();
    return Status::OK;
}

StorageEngine::StorageEngine(const EngineOptions& options)
        : _options(options),
        _available_storage_medium_type_count(0),
        _effective_cluster_id(-1),
        _is_all_cluster_id_exist(true),
        _is_drop_tables(false),
        _index_stream_lru_cache(NULL),
        _is_report_disk_state_already(false),
        _is_report_tablet_already(false){
    if (_s_instance == nullptr) {
        _s_instance = this;
    }
}

StorageEngine::~StorageEngine() {
    clear();
}

// convert old tablet and its files to new tablet meta and rowset format
// if any error occurred during converting, stop it and break.
OLAPStatus StorageEngine::_convert_old_tablet(DataDir* data_dir) {
    auto convert_tablet_func = [this, data_dir](long tablet_id,
        long schema_hash, const std::string& value) -> bool {
        OlapSnapshotConverter converter;
        // convert olap header and files
        OLAPHeaderMessage olap_header_msg;
        TabletMetaPB tablet_meta_pb;
        vector<RowsetMetaPB> pending_rowsets;
        bool parsed = olap_header_msg.ParseFromString(value);
        if (!parsed) {
            LOG(WARNING) << "convert olap header to tablet meta failed when load olap header tablet=" 
                         << tablet_id << "." << schema_hash;
            return false;
        }
        string old_data_path_prefix = data_dir->get_absolute_tablet_path(olap_header_msg, true);
        OLAPStatus status = converter.to_new_snapshot(olap_header_msg, old_data_path_prefix, 
            old_data_path_prefix, *data_dir, &tablet_meta_pb, &pending_rowsets);
        if (status != OLAP_SUCCESS) {
            LOG(WARNING) << "convert olap header to tablet meta failed when convert header and files tablet=" 
                         << tablet_id << "." << schema_hash;
            return false;
        }

        // write pending rowset to olap meta
        for (auto& rowset_pb : pending_rowsets) {
            string meta_binary;
            rowset_pb.SerializeToString(&meta_binary);
            status = RowsetMetaManager::save(data_dir->get_meta(), rowset_pb.rowset_id() , meta_binary);
            if (status != OLAP_SUCCESS) {
                LOG(WARNING) << "convert olap header to tablet meta failed when save rowset meta tablet=" 
                             << tablet_id << "." << schema_hash;
                return false;
            }
        }

        // write converted tablet meta to olap meta
        string meta_binary;
        tablet_meta_pb.SerializeToString(&meta_binary);
        status = TabletMetaManager::save(data_dir, tablet_meta_pb.tablet_id(), tablet_meta_pb.schema_hash(), meta_binary);
        if (status != OLAP_SUCCESS) {
            LOG(WARNING) << "convert olap header to tablet meta failed when save tablet meta tablet=" 
                         << tablet_id << "." << schema_hash;
            return false;
        } else {
            LOG(INFO) << "convert olap header to tablet meta successfully and save tablet meta to meta tablet=" 
                      << tablet_id << "." << schema_hash;
        }
        return true;
    };
    OLAPStatus convert_tablet_status = TabletMetaManager::traverse_headers(data_dir->get_meta(), 
        convert_tablet_func, OLD_HEADER_PREFIX);
    if (convert_tablet_status != OLAP_SUCCESS) {
        LOG(WARNING) << "there is failure when convert old tablet, data dir:" << data_dir->path();
        return convert_tablet_status;
    } else {
        LOG(INFO) << "successfully convert old tablet, data dir: " << data_dir->path();
    }
    return OLAP_SUCCESS;
}

OLAPStatus StorageEngine::_clean_unfinished_converting_data(DataDir* data_dir) {
    auto clean_unifinished_tablet_meta_func = [this, data_dir](long tablet_id,
        long schema_hash, const std::string& value) -> bool {
        TabletMetaManager::remove(data_dir, tablet_id, schema_hash, HEADER_PREFIX);
        LOG(INFO) << "successfully clean temp tablet meta for tablet=" 
                  << tablet_id << "." << schema_hash 
                  << "from data dir: " << data_dir->path();
        return true;
    };
    OLAPStatus clean_unfinished_meta_status = TabletMetaManager::traverse_headers(data_dir->get_meta(), 
        clean_unifinished_tablet_meta_func, HEADER_PREFIX);
    if (clean_unfinished_meta_status != OLAP_SUCCESS) {
        // If failed to clean meta just skip the error, there will be useless metas in rocksdb column family
        LOG(WARNING) << "there is failure when clean temp tablet meta from data dir:" << data_dir->path();
    } else {
        LOG(INFO) << "successfully clean temp tablet meta from data dir: " << data_dir->path();
    }
    auto clean_unifinished_rowset_meta_func = [this, data_dir](RowsetId rowset_id, const std::string& value) -> bool {
        RowsetMetaManager::remove(data_dir->get_meta(), rowset_id);
        LOG(INFO) << "successfully clean temp rowset meta for rowset id =" 
                  << rowset_id << " from data dir: " << data_dir->path();
        return true;
    };
    OLAPStatus clean_unfinished_rowset_meta_status = RowsetMetaManager::traverse_rowset_metas(data_dir->get_meta(), 
        clean_unifinished_rowset_meta_func);
    if (clean_unfinished_rowset_meta_status != OLAP_SUCCESS) {
        // If failed to clean meta just skip the error, there will be useless metas in rocksdb column family
        LOG(WARNING) << "there is failure when clean temp rowset meta from data dir:" << data_dir->path();
    } else {
        LOG(INFO) << "successfully clean temp rowset meta from data dir: " << data_dir->path();
    }
    return OLAP_SUCCESS;
}

OLAPStatus StorageEngine::_remove_old_meta_and_files(DataDir* data_dir) {
    // clean old meta(olap header message) 
    auto clean_old_meta_func = [this, data_dir](long tablet_id,
        long schema_hash, const std::string& value) -> bool {
        TabletMetaManager::remove(data_dir, tablet_id, schema_hash, OLD_HEADER_PREFIX);
        LOG(INFO) << "successfully clean old tablet meta(olap header) for tablet=" 
                  << tablet_id << "." << schema_hash 
                  << "from data dir: " << data_dir->path();
        return true;
    };
    OLAPStatus clean_old_meta_status = TabletMetaManager::traverse_headers(data_dir->get_meta(), 
        clean_old_meta_func, OLD_HEADER_PREFIX);
    if (clean_old_meta_status != OLAP_SUCCESS) {
        // If failed to clean meta just skip the error, there will be useless metas in rocksdb column family
        LOG(WARNING) << "there is failure when clean old tablet meta(olap header) from data dir:" << data_dir->path();
    } else {
        LOG(INFO) << "successfully clean old tablet meta(olap header) from data dir: " << data_dir->path();
    }

    // clean old files because they have hard links in new file name format
    auto clean_old_files_func = [this, data_dir](long tablet_id,
        long schema_hash, const std::string& value) -> bool {
        TabletMetaPB tablet_meta_pb;
        bool parsed = tablet_meta_pb.ParseFromString(value);
        if (!parsed) {
            // if errors when load, just skip it
            LOG(WARNING) << "failed to load tablet meta from meta store to tablet=" << tablet_id << "." << schema_hash;
            return true;
        }

        TabletSchema tablet_schema;
        tablet_schema.init_from_pb(tablet_meta_pb.schema());
        string data_path_prefix = data_dir->get_absolute_tablet_path(&tablet_meta_pb, true);

        // convert visible pdelta file to rowsets and remove old files
        for (auto& visible_rowset : tablet_meta_pb.rs_metas()) {
            RowsetMetaSharedPtr alpha_rowset_meta(new AlphaRowsetMeta());
            alpha_rowset_meta->init_from_pb(visible_rowset);
            AlphaRowset rowset(&tablet_schema, data_path_prefix, data_dir, alpha_rowset_meta);
            rowset.init();
            std::vector<std::string> old_files;
            rowset.remove_old_files(&old_files);
        }

        // convert inc delta file to rowsets and remove old files
        for (auto& inc_rowset : tablet_meta_pb.inc_rs_metas()) {
            RowsetMetaSharedPtr alpha_rowset_meta(new AlphaRowsetMeta());
            alpha_rowset_meta->init_from_pb(inc_rowset);
            AlphaRowset rowset(&tablet_schema, data_path_prefix, data_dir, alpha_rowset_meta);
            rowset.init();
            std::vector<std::string> old_files;
            rowset.remove_old_files(&old_files);
        }
        return true;
    };
    OLAPStatus clean_old_tablet_status = TabletMetaManager::traverse_headers(data_dir->get_meta(), 
        clean_old_files_func, HEADER_PREFIX);
    if (clean_old_tablet_status != OLAP_SUCCESS) {
        LOG(WARNING) << "there is failure when loading tablet and clean old files:" << data_dir->path();
    } else {
        LOG(INFO) << "load rowset from meta finished, data dir: " << data_dir->path();
    }
    return OLAP_SUCCESS;
}

// TODO(ygl): deal with rowsets and tablets when load failed
OLAPStatus StorageEngine::_load_data_dir(DataDir* data_dir) {
    // check if this is an old data path
    bool is_tablet_convert_finished = false;
    OLAPStatus res = data_dir->get_meta()->get_tablet_convert_finished(is_tablet_convert_finished);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "get convert flag from meta failed dir = " << data_dir->path();
        return res;
    }

    if (!is_tablet_convert_finished) {
        _clean_unfinished_converting_data(data_dir);
        res = _convert_old_tablet(data_dir);
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "convert old tablet failed for  dir = " << data_dir->path();
            return res;
        }
        // TODO(ygl): should load tablet successfully and then set convert flag and clean old files
        res = data_dir->get_meta()->set_tablet_convert_finished();
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "save convert flag failed after convert old tablet. " 
                         << " dir = " << data_dir->path();
            return res;
        }
        // convert may be successfully, but crashed before remove old files
        // depend on gc thread to recycle the old files
        // _remove_old_meta_and_files(data_dir);
    } else {
        LOG(INFO) << "tablets have been converted, skip convert process";
    }

    std::string data_dir_path = data_dir->path();
    LOG(INFO) << "start to load tablets from data_dir_path:" << data_dir_path;
    // load rowset meta from meta env and create rowset
    // COMMITTED: add to txn manager
    // VISIBLE: add to tablet
    // if one rowset load failed, then the total data dir will not be loaded
    std::vector<RowsetMetaSharedPtr> dir_rowset_metas;
    LOG(INFO) << "begin loading rowset from meta";
    auto load_rowset_func = [this, data_dir, &dir_rowset_metas](RowsetId rowset_id, const std::string& meta_str) -> bool {
        RowsetMetaSharedPtr rowset_meta(new AlphaRowsetMeta());
        bool parsed = rowset_meta->init(meta_str);
        if (!parsed) {
            LOG(WARNING) << "parse rowset meta string failed for rowset_id:" << rowset_id;
            // return false will break meta iterator, return true to skip this error
            return true;
        }
        dir_rowset_metas.push_back(rowset_meta);
        return true;
    };
    OLAPStatus load_rowset_status = RowsetMetaManager::traverse_rowset_metas(data_dir->get_meta(), load_rowset_func);

    if (load_rowset_status != OLAP_SUCCESS) {
        LOG(WARNING) << "errors when load rowset meta from meta env, skip this data dir:" << data_dir_path;
    } else {
        LOG(INFO) << "load rowset from meta finished, data dir: " << data_dir_path;
    }

    // load tablet
    // create tablet from tablet meta and add it to tablet mgr
    LOG(INFO) << "begin loading tablet from meta";
    auto load_tablet_func = [this, data_dir](long tablet_id,
        long schema_hash, const std::string& value) -> bool {
        OLAPStatus status = TabletManager::instance()->load_tablet_from_meta(data_dir, tablet_id, schema_hash, value);
        if (status != OLAP_SUCCESS) {
            LOG(WARNING) << "load tablet from header failed. status:" << status
                << ", tablet=" << tablet_id << "." << schema_hash;
        };
        return true;
    };
    OLAPStatus load_tablet_status = TabletMetaManager::traverse_headers(data_dir->get_meta(), load_tablet_func);
    if (load_tablet_status != OLAP_SUCCESS) {
        LOG(WARNING) << "there is failure when loading tablet headers, path:" << data_dir_path;
    } else {
        LOG(INFO) << "load rowset from meta finished, data dir: " << data_dir_path;
    }

    // tranverse rowset
    // 1. add committed rowset to txn map
    // 2. add visible rowset to tablet
    // ignore any errors when load tablet or rowset, because fe will repair them after report
    for (auto rowset_meta : dir_rowset_metas) {
        TabletSharedPtr tablet = TabletManager::instance()->get_tablet(rowset_meta->tablet_id(), rowset_meta->tablet_schema_hash());
        // tablet maybe dropped, but not drop related rowset meta
        if (tablet.get() == NULL) {
            LOG(WARNING) << "could not find tablet id: " << rowset_meta->tablet_id()
                         << ", schema hash: " << rowset_meta->tablet_schema_hash()
                         << ", for rowset: " << rowset_meta->rowset_id()
                         << ", skip this rowset";
            continue;
        }
        RowsetSharedPtr rowset;
        OLAPStatus create_status = RowsetFactory::load_rowset(tablet->tablet_schema(), 
                                                             rowset_meta->rowset_path(), 
                                                             tablet->data_dir(), 
                                                             rowset_meta, &rowset);
        if (create_status != OLAP_SUCCESS) {
            LOG(WARNING) << "could not create rowset from rowsetmeta: "
                         << " rowset_id: " << rowset_meta->rowset_id()
                         << " rowset_type: " << rowset_meta->rowset_type()
                         << " rowset_state: " << rowset_meta->rowset_state();
            continue;
        }
        if (rowset_meta->rowset_state() == RowsetStatePB::COMMITTED) {
            OLAPStatus commit_txn_status = TxnManager::instance()->commit_txn(
                tablet->data_dir()->get_meta(),
                rowset_meta->partition_id(), rowset_meta->txn_id(), 
                rowset_meta->tablet_id(), rowset_meta->tablet_schema_hash(), 
                rowset_meta->load_id(), rowset, true);
            if (commit_txn_status != OLAP_SUCCESS && commit_txn_status != OLAP_ERR_PUSH_TRANSACTION_ALREADY_EXIST) {
                LOG(WARNING) << "failed to add committed rowset: " << rowset_meta->rowset_id()
                             << " to tablet: " << rowset_meta->tablet_id() 
                             << " for txn: " << rowset_meta->txn_id();
            } else {
                LOG(INFO) << "successfully to add committed rowset: " << rowset_meta->rowset_id()
                             << " to tablet: " << rowset_meta->tablet_id() 
                             << " schema hash: " << rowset_meta->tablet_schema_hash()
                             << " for txn: " << rowset_meta->txn_id();
            }
        } else if (rowset_meta->rowset_state() == RowsetStatePB::VISIBLE) {
            // add visible rowset to tablet, it maybe use in the future
            // there should be only preparing rowset in meta env because visible 
            // rowset is persist with tablet meta currently
            OLAPStatus publish_status = tablet->add_inc_rowset(rowset);
            if (publish_status != OLAP_SUCCESS) {
                LOG(WARNING) << "add visilbe rowset to tablet failed rowset_id:" << rowset->rowset_id()
                             << " tablet id: " << rowset_meta->tablet_id()
                             << " txn id:" << rowset_meta->txn_id()
                             << " start_version: " << rowset_meta->version().first
                             << " end_version: " << rowset_meta->version().second;
            } else {
                LOG(INFO) << "successfully to add visible rowset: " << rowset_meta->rowset_id()
                          << " to tablet: " << rowset_meta->tablet_id()
                          << " txn id:" << rowset_meta->txn_id()
                          << " start_version: " << rowset_meta->version().first
                          << " end_version: " << rowset_meta->version().second;
            }
        } else {
            LOG(WARNING) << "find invalid rowset: " << rowset_meta->rowset_id()
                         << " with tablet id: " << rowset_meta->tablet_id() 
                         << " schema hash: " << rowset_meta->tablet_schema_hash()
                         << " txn: " << rowset_meta->txn_id();
        }
    }
    return OLAP_SUCCESS;
}

void StorageEngine::load_data_dirs(const std::vector<DataDir*>& data_dirs) {
    std::vector<std::thread> threads;
    for (auto data_dir : data_dirs) {
        threads.emplace_back([this, data_dir] {
            auto res = _load_data_dir(data_dir);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "io error when init load tables. res=" << res
                    << ", data dir=" << data_dir->path();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

OLAPStatus StorageEngine::open() {
    // init store_map
    for (auto& path : _options.store_paths) {
        DataDir* store = new DataDir(path.path, path.capacity_bytes);
        auto st = store->init();
        if (!st.ok()) {
            LOG(WARNING) << "Store load failed, path=" << path.path;
            return OLAP_ERR_INVALID_ROOT_PATH;
        }
        _store_map.emplace(path.path, store);
    }
    _effective_cluster_id = config::cluster_id;
    auto res = check_all_root_path_cluster_id();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to check cluster info. res=" << res;
        return res;
    }

    _update_storage_medium_type_count();

    auto cache = new_lru_cache(config::file_descriptor_cache_capacity);
    if (cache == nullptr) {
        OLAP_LOG_WARNING("failed to init file descriptor LRUCache");
        TabletManager::instance()->clear();
        return OLAP_ERR_INIT_FAILED;
    }
    FileHandler::set_fd_cache(cache);

    // 初始化LRUCache
    // cache大小可通过配置文件配置
    _index_stream_lru_cache = new_lru_cache(config::index_stream_cache_capacity);
    if (_index_stream_lru_cache == NULL) {
        OLAP_LOG_WARNING("failed to init index stream LRUCache");
        TabletManager::instance()->clear();
        return OLAP_ERR_INIT_FAILED;
    }

    // 初始化CE调度器
    int32_t cumulative_compaction_num_threads = config::cumulative_compaction_num_threads;
    int32_t base_compaction_num_threads = config::base_compaction_num_threads;
    uint32_t file_system_num = get_file_system_count();
    _max_cumulative_compaction_task_per_disk = (cumulative_compaction_num_threads + file_system_num - 1) / file_system_num;
    _max_base_compaction_task_per_disk = (base_compaction_num_threads + file_system_num - 1) / file_system_num;

    auto dirs = get_stores();
    load_data_dirs(dirs);
    // 取消未完成的SchemaChange任务
    TabletManager::instance()->cancel_unfinished_schema_change();

    return OLAP_SUCCESS;
}

void StorageEngine::_update_storage_medium_type_count() {
    set<TStorageMedium::type> available_storage_medium_types;

    std::lock_guard<std::mutex> l(_store_lock);
    for (auto& it : _store_map) {
        if (it.second->is_used()) {
            available_storage_medium_types.insert(it.second->storage_medium());
        }
    }

    _available_storage_medium_type_count = available_storage_medium_types.size();
    TabletManager::instance()->update_storage_medium_type_count(_available_storage_medium_type_count);
}


OLAPStatus StorageEngine::_judge_and_update_effective_cluster_id(int32_t cluster_id) {
    OLAPStatus res = OLAP_SUCCESS;

    if (cluster_id == -1 && _effective_cluster_id == -1) {
        // maybe this is a new cluster, cluster id will get from heartbeate
        return res;
    } else if (cluster_id != -1 && _effective_cluster_id == -1) {
        _effective_cluster_id = cluster_id;
    } else if (cluster_id == -1 && _effective_cluster_id != -1) {
        // _effective_cluster_id is the right effective cluster id
        return res;
    } else {
        if (cluster_id != _effective_cluster_id) {
            OLAP_LOG_WARNING("multiple cluster ids is not equal. [id1=%d id2=%d]",
                             _effective_cluster_id, cluster_id);
            return OLAP_ERR_INVALID_CLUSTER_INFO;
        }
    }

    return res;
}

void StorageEngine::set_store_used_flag(const string& path, bool is_used) {
    std::lock_guard<std::mutex> l(_store_lock);
    auto it = _store_map.find(path);
    if (it == _store_map.end()) {
        LOG(WARNING) << "store not exist, path=" << path;
    }

    it->second->set_is_used(is_used);
    _update_storage_medium_type_count();
}

void StorageEngine::get_all_available_root_path(std::vector<std::string>* available_paths) {
    available_paths->clear();
    std::lock_guard<std::mutex> l(_store_lock);
    for (auto& it : _store_map) {
        if (it.second->is_used()) {
            available_paths->push_back(it.first);
        }
    }
}

template<bool include_unused>
std::vector<DataDir*> StorageEngine::get_stores() {
    std::vector<DataDir*> stores;
    stores.reserve(_store_map.size());

    std::lock_guard<std::mutex> l(_store_lock);
    if (include_unused) {
        for (auto& it : _store_map) {
            stores.push_back(it.second);
        }
    } else {
        for (auto& it : _store_map) {
            if (it.second->is_used()) {
                stores.push_back(it.second);
            }
        }
    }
    return stores;
}

template std::vector<DataDir*> StorageEngine::get_stores<false>();
template std::vector<DataDir*> StorageEngine::get_stores<true>();

OLAPStatus StorageEngine::get_all_data_dir_info(vector<DataDirInfo>* data_dir_infos) {
    OLAPStatus res = OLAP_SUCCESS;
    data_dir_infos->clear();

    MonotonicStopWatch timer;
    timer.start();
    int tablet_counter = 0;

    // get all root path info and construct a path map.
    // path -> DataDirInfo
    std::map<std::string, DataDirInfo> path_map;
    {
        std::lock_guard<std::mutex> l(_store_lock);
        for (auto& it : _store_map) {
            std::string path = it.first;
            path_map.emplace(path, it.second->get_dir_info());
            // if this path is not used, init it's info
            if (!path_map[path].is_used) {
                path_map[path].capacity = 1;
                path_map[path].data_used_capacity = 0;
                path_map[path].available = 0;
                path_map[path].storage_medium = TStorageMedium::HDD;
            } else {
                path_map[path].storage_medium = it.second->storage_medium();
            }
        }
    }

    // for each tablet, get it's data size, and accumulate the path 'data_used_capacity'
    // which the tablet belongs to.
    TabletManager::instance()->update_root_path_info(&path_map, &tablet_counter);

    // add path info to data_dir_infos
    for (auto& entry : path_map) {
        data_dir_infos->emplace_back(entry.second);
    }

    // get available capacity of each path
    for (auto& info: *data_dir_infos) {
        if (info.is_used) {
            _get_path_available_capacity(info.path,  &info.available);
        }
    }
    timer.stop();
    LOG(INFO) << "get root path info cost: " << timer.elapsed_time() / 1000000
            << " ms. tablet counter: " << tablet_counter;

    return res;
}

void StorageEngine::start_disk_stat_monitor() {
    for (auto& it : _store_map) {
        it.second->health_check();
    }
    _update_storage_medium_type_count();
    _delete_tables_on_unused_root_path();
    
    // if drop tables
    // notify disk_state_worker_thread and tablet_worker_thread until they received
    if (_is_drop_tables) {
        report_notify(true);

        bool is_report_disk_state_expected = true;
        bool is_report_tablet_expected = true;
        bool is_report_disk_state_exchanged =
                _is_report_disk_state_already.compare_exchange_strong(is_report_disk_state_expected, false);
        bool is_report_tablet_exchanged =
                _is_report_tablet_already.compare_exchange_strong(is_report_tablet_expected, false);
        if (is_report_disk_state_exchanged && is_report_tablet_exchanged) {
            _is_drop_tables = false;
        }
    }
}

bool StorageEngine::_used_disk_not_enough(uint32_t unused_num, uint32_t total_num) {
    return ((total_num == 0) || (unused_num * 100 / total_num > _min_percentage_of_error_disk));
}

OLAPStatus StorageEngine::check_all_root_path_cluster_id() {
    int32_t cluster_id = -1;
    for (auto& it : _store_map) {
        int32_t tmp_cluster_id = it.second->cluster_id();
        if (tmp_cluster_id == -1) {
            _is_all_cluster_id_exist = false;
        } else if (tmp_cluster_id == cluster_id) {
            // both hava right cluster id, do nothing
        } else if (cluster_id == -1) {
            cluster_id = tmp_cluster_id;
        } else {
            LOG(WARNING) << "multiple cluster ids is not equal. one=" << cluster_id
                << ", other=" << tmp_cluster_id;
            return OLAP_ERR_INVALID_CLUSTER_INFO;
        }
    }

    // judge and get effective cluster id
    OLAPStatus res = OLAP_SUCCESS;
    res = _judge_and_update_effective_cluster_id(cluster_id);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to judge and update effective cluster id. [res=%d]", res);
        return res;
    }

    // write cluster id into cluster_id_path if get effective cluster id success
    if (_effective_cluster_id != -1 && !_is_all_cluster_id_exist) {
        set_cluster_id(_effective_cluster_id);
    }

    return res;
}

Status StorageEngine::set_cluster_id(int32_t cluster_id) {
    std::lock_guard<std::mutex> l(_store_lock);
    for (auto& it : _store_map) {
        RETURN_IF_ERROR(it.second->set_cluster_id(cluster_id));
    }
    _effective_cluster_id = cluster_id;
    _is_all_cluster_id_exist = true;
    return Status::OK;
}

std::vector<DataDir*> StorageEngine::get_stores_for_create_tablet(
        TStorageMedium::type storage_medium) {
    std::vector<DataDir*> stores;
    {
        std::lock_guard<std::mutex> l(_store_lock);
        for (auto& it : _store_map) {
            if (it.second->is_used()) {
                if (_available_storage_medium_type_count == 1
                    || it.second->storage_medium() == storage_medium) {
                    stores.push_back(it.second);
                }
            }
        }
    }
    std::random_device rd;
    srand(rd());
    std::random_shuffle(stores.begin(), stores.end());
    return stores;
}

DataDir* StorageEngine::get_store(const std::string& path) {
    std::lock_guard<std::mutex> l(_store_lock);
    auto it = _store_map.find(path);
    if (it == std::end(_store_map)) {
        return nullptr;
    }
    return it->second;
}

void StorageEngine::_delete_tables_on_unused_root_path() {
    vector<TabletInfo> tablet_info_vec;
    uint32_t unused_root_path_num = 0;
    uint32_t total_root_path_num = 0;

    std::lock_guard<std::mutex> l(_store_lock);

    for (auto& it : _store_map) {
        total_root_path_num++;
        if (it.second->is_used()) {
            continue;
        }
        it.second->clear_tablets(&tablet_info_vec);
    }

    if (_used_disk_not_enough(unused_root_path_num, total_root_path_num)) {
        LOG(FATAL) << "engine stop running, because more than " << _min_percentage_of_error_disk
                   << " disks error. total_disks=" << total_root_path_num
                   << ", error_disks=" << unused_root_path_num;
        exit(0);
    }

    if (!tablet_info_vec.empty()) {
        _is_drop_tables = true;
    }
    
    TabletManager::instance()->drop_tablets_on_error_root_path(tablet_info_vec);
}

OLAPStatus StorageEngine::_get_path_available_capacity(
        const string& root_path,
        int64_t* disk_available) {
    OLAPStatus res = OLAP_SUCCESS;

    try {
        boost::filesystem::path path_name(root_path);
        boost::filesystem::space_info path_info = boost::filesystem::space(path_name);
        *disk_available = path_info.available;
    } catch (boost::filesystem::filesystem_error& e) {
        LOG(WARNING) << "get space info failed. path: " << root_path << " erro:" << e.what();
        return OLAP_ERR_STL_ERROR;
    }

    return res;
}

OLAPStatus StorageEngine::clear() {
    // 删除lru中所有内容,其实进程退出这么做本身意义不大,但对单测和更容易发现问题还是有很大意义的
    delete FileHandler::get_fd_cache();
    FileHandler::set_fd_cache(nullptr);
    SAFE_DELETE(_index_stream_lru_cache);

    return OLAP_SUCCESS;
}

void StorageEngine::clear_transaction_task(const TTransactionId transaction_id,
                                        const vector<TPartitionId> partition_ids) {
    LOG(INFO) << "begin to clear transaction task. transaction_id=" <<  transaction_id;

    for (const TPartitionId& partition_id : partition_ids) {
        std::map<TabletInfo, RowsetSharedPtr> tablet_infos;
        TxnManager::instance()->get_txn_related_tablets(transaction_id, partition_id, &tablet_infos);

        // each tablet
        for (auto& tablet_info : tablet_infos) {
            TabletSharedPtr tablet = TabletManager::instance()->get_tablet(tablet_info.first.tablet_id, 
                tablet_info.first.schema_hash, false);
            OlapMeta* meta = nullptr;
            if (tablet != NULL) {
                meta = tablet->data_dir()->get_meta();
            }
            TxnManager::instance()->delete_txn(meta, partition_id, transaction_id,
                                tablet_info.first.tablet_id, tablet_info.first.schema_hash);
        }
    }
    LOG(INFO) << "finish to clear transaction task. transaction_id=" << transaction_id;
}

TabletSharedPtr StorageEngine::create_tablet(const TCreateTabletReq& request,
                                             const bool is_schema_change_tablet,
                                             const TabletSharedPtr ref_tablet) {
    // Get all available stores, use data_dir of ref_tablet when doing schema change
    std::vector<DataDir*> stores;
    if (!is_schema_change_tablet) {
        stores = get_stores_for_create_tablet(request.storage_medium);
        if (stores.empty()) {
            LOG(WARNING) << "there is no available disk that can be used to create tablet.";
            return nullptr;
        }
    } else {
        stores.push_back(ref_tablet->data_dir());
    }

    return TabletManager::instance()->create_tablet(request, is_schema_change_tablet, ref_tablet, stores);
}

void StorageEngine::start_clean_fd_cache() {
    VLOG(10) << "start clean file descritpor cache";
    FileHandler::get_fd_cache()->prune();
    VLOG(10) << "end clean file descritpor cache";
}

void StorageEngine::perform_cumulative_compaction(OlapStore* store) {
    TabletSharedPtr best_tablet = _find_best_tablet_to_compaction(CompactionType::CUMULATIVE_COMPACTION, store);
    if (best_tablet == nullptr) { return; }

    DorisMetrics::cumulative_compaction_request_total.increment(1);
    CumulativeCompaction cumulative_compaction;
    OLAPStatus res = cumulative_compaction.init(best_tablet);
    if (res != OLAP_SUCCESS) {
        if (res != OLAP_ERR_CUMULATIVE_REPEAT_INIT && res != OLAP_ERR_CE_TRY_CE_LOCK_ERROR) {
            best_tablet->set_last_compaction_failure_time(UnixMillis());
            LOG(WARNING) << "failed to init cumulative compaction"
                << ", table=" << best_tablet->full_name()
                << ", res=" << res;

            if (res != OLAP_ERR_CUMULATIVE_NO_SUITABLE_VERSIONS) {
                DorisMetrics::cumulative_compaction_request_failed.increment(1);
            }
        }
        return;
    }

    res = cumulative_compaction.run();
    if (res != OLAP_SUCCESS) {
        DorisMetrics::cumulative_compaction_request_failed.increment(1);
        best_tablet->set_last_compaction_failure_time(UnixMillis());
        LOG(WARNING) << "failed to do cumulative compaction"
                     << ", table=" << best_tablet->full_name()
                     << ", res=" << res;
        return;
    }
    best_tablet->set_last_compaction_failure_time(0);
}

void StorageEngine::perform_base_compaction(OlapStore* store) {
    TabletSharedPtr best_tablet = _find_best_tablet_to_compaction(CompactionType::BASE_COMPACTION, store);
    if (best_tablet == nullptr) { return; }

    DorisMetrics::base_compaction_request_total.increment(1);
    BaseCompaction base_compaction;
    OLAPStatus res = base_compaction.init(best_tablet);
    if (res != OLAP_SUCCESS) {
        if (res != OLAP_ERR_BE_TRY_BE_LOCK_ERROR && res != OLAP_ERR_BE_NO_SUITABLE_VERSION) {
            DorisMetrics::base_compaction_request_failed.increment(1);
            best_tablet->set_last_compaction_failure_time(UnixMillis());
            LOG(WARNING) << "failed to init base compaction"
                << ", table=" << best_tablet->full_name()
                << ", res=" << res;
        }
        return;
    }

    res = base_compaction.run();
    if (res != OLAP_SUCCESS) {
        DorisMetrics::base_compaction_request_failed.increment(1);
        best_tablet->set_last_compaction_failure_time(UnixMillis());
        LOG(WARNING) << "failed to init base compaction"
                     << ", table=" << best_tablet->full_name()
                     << ", res=" << res;
        return;
    }
    best_tablet->set_last_compaction_failure_time(0);
}

OLAPTablePtr StorageEngine::_find_best_tablet_to_compaction(CompactionType compaction_type, OlapStore* store) {
    ReadLock tablet_map_rdlock(&_tablet_map_lock);
    uint32_t highest_score = 0;
    TabletSharedPtr best_tablet;
    int64_t now = UnixMillis();
    for (tablet_map_t::value_type& table_ins : _tablet_map){
        for (OLAPTablePtr& table_ptr : table_ins.second.table_arr) {
            if (table_ptr->store()->path_hash() != store->path_hash() 
                || !table_ptr->is_used() || !table_ptr->is_loaded() || !_can_do_compaction(table_ptr)) {
                continue;
            }
          
            if (now - table_ptr->last_compaction_failure_time() <= config::min_compaction_failure_interval_sec * 1000) {
                LOG(INFO) << "tablet last compaction failure time is: " << table_ptr->last_compaction_failure_time()
                        << ", tablet: " << table_ptr->tablet_id() << ", skip it.";
                continue;
            }

            if (compaction_type == CompactionType::CUMULATIVE_COMPACTION) {
                if (!table_ptr->try_cumulative_lock()) {
                    continue;
                } else {
                    table_ptr->release_cumulative_lock();
                }
            }

            if (compaction_type == CompactionType::BASE_COMPACTION) {
                if (!table_ptr->try_base_compaction_lock()) {
                    continue;
                } else {
                    table_ptr->release_base_compaction_lock();
                }
            } 

            ReadLock rdlock(table_ptr->get_header_lock_ptr());
            uint32_t table_score = 0;
            if (compaction_type == CompactionType::BASE_COMPACTION) {
                table_score = table_ptr->get_base_compaction_score();
            } else if (compaction_type == CompactionType::CUMULATIVE_COMPACTION) {
                table_score = table_ptr->get_cumulative_compaction_score();
            }
            if (table_score > highest_score) {
                highest_score = table_score;
                best_tablet = table_ptr;
            }
        }
    }

    if (best_tablet != nullptr) {
        LOG(INFO) << "find best tablet to do compaction. type: " << (compaction_type == CompactionType::CUMULATIVE_COMPACTION ? "cumulative" : "base")
            << ", tablet id: " << best_table->tablet_id() << ", score: " << highest_score;
    }
    return best_tablet;
}

void StorageEngine::get_cache_status(rapidjson::Document* document) const {
    return _index_stream_lru_cache->get_cache_status(document);
}

OLAPStatus StorageEngine::start_trash_sweep(double* usage) {
    OLAPStatus res = OLAP_SUCCESS;
    LOG(INFO) << "start trash and snapshot sweep.";

    const uint32_t snapshot_expire = config::snapshot_expire_time_sec;
    const uint32_t trash_expire = config::trash_file_expire_time_sec;
    const double guard_space = config::disk_capacity_insufficient_percentage / 100.0;
    std::vector<DataDirInfo> data_dir_infos;
    res = get_all_data_dir_info(&data_dir_infos);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("failed to get root path stat info when sweep trash.");
        return res;
    }

    time_t now = time(NULL); //获取UTC时间
    tm local_tm_now;
    if (localtime_r(&now, &local_tm_now) == NULL) {
        OLAP_LOG_WARNING("fail to localtime_r time. [time=%lu]", now);
        return OLAP_ERR_OS_ERROR;
    }
    const time_t local_now = mktime(&local_tm_now); //得到当地日历时间

    for (DataDirInfo& info : data_dir_infos) {
        if (!info.is_used) {
            continue;
        }

        double curr_usage = (info.capacity - info.available)
                / (double) info.capacity;
        *usage = *usage > curr_usage ? *usage : curr_usage;

        OLAPStatus curr_res = OLAP_SUCCESS;
        string snapshot_path = info.path + SNAPSHOT_PREFIX;
        curr_res = _do_sweep(snapshot_path, local_now, snapshot_expire);
        if (curr_res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("failed to sweep snapshot. [path=%s, err_code=%d]",
                    snapshot_path.c_str(), curr_res);
            res = curr_res;
        }

        string trash_path = info.path + TRASH_PREFIX;
        curr_res = _do_sweep(trash_path, local_now,
                curr_usage > guard_space ? 0 : trash_expire);
        if (curr_res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("failed to sweep trash. [path=%s, err_code=%d]",
                    trash_path.c_str(), curr_res);
            res = curr_res;
        }
    }

    // clear expire incremental segment_group
    TabletManager::instance()->start_trash_sweep();

    return res;
}

OLAPStatus StorageEngine::_do_sweep(
        const string& scan_root, const time_t& local_now, const uint32_t expire) {
    OLAPStatus res = OLAP_SUCCESS;
    if (!check_dir_existed(scan_root)) {
        // dir not existed. no need to sweep trash.
        return res;
    }

    try {
        path boost_scan_root(scan_root);
        directory_iterator item(boost_scan_root);
        directory_iterator item_end;
        for (; item != item_end; ++item) {
            string path_name = item->path().string();
            string dir_name = item->path().filename().string();
            string str_time = dir_name.substr(0, dir_name.find('.'));
            tm local_tm_create;
            if (strptime(str_time.c_str(), "%Y%m%d%H%M%S", &local_tm_create) == nullptr) {
                LOG(WARNING) << "fail to strptime time. [time=" << str_time << "]";
                res = OLAP_ERR_OS_ERROR;
                continue;
            }
            if (difftime(local_now, mktime(&local_tm_create)) >= expire) {
                if (remove_all_dir(path_name) != OLAP_SUCCESS) {
                    OLAP_LOG_WARNING("fail to remove file or directory. [path=%s]",
                            path_name.c_str());
                    res = OLAP_ERR_OS_ERROR;
                    continue;
                }
            }
        }
    } catch (...) {
        OLAP_LOG_WARNING("Exception occur when scan directory. [path=%s]",
                scan_root.c_str());
        res = OLAP_ERR_IO_ERROR;
    }

    return res;
}

void StorageEngine::start_delete_unused_index() {
    _gc_mutex.lock();

    for (auto it = _gc_files.begin(); it != _gc_files.end();) {
        if (it->first->is_in_use()) {
            ++it;
        } else {
            delete it->first;
            vector<string> files = it->second;
            remove_files(files);
            it = _gc_files.erase(it);
        }
    }

    _gc_mutex.unlock();
}

void StorageEngine::add_unused_index(SegmentGroup* segment_group) {
    _gc_mutex.lock();

    auto it = _gc_files.find(segment_group);
    if (it == _gc_files.end()) {
        vector<string> files;
        for (size_t seg_id = 0; seg_id < segment_group->num_segments(); ++seg_id) {
            string index_file = segment_group->construct_index_file_path(seg_id);
            files.push_back(index_file);

            string data_file = segment_group->construct_data_file_path(seg_id);
            files.push_back(data_file);
        }
        _gc_files[segment_group] = files;
    }

    _gc_mutex.unlock();
}

void StorageEngine::start_delete_unused_rowset() {
    _gc_mutex.lock();

    auto it = _unused_rowsets.begin();
    for (; it != _unused_rowsets.end();) { 
        if (it->second->in_use()) {
            ++it;
        } else {
            it->second->remove();
            _unused_rowsets.erase(it);
        }
    }

    _gc_mutex.unlock();
}

void StorageEngine::add_unused_rowset(RowsetSharedPtr rowset) {
    _gc_mutex.lock();
    auto it = _unused_rowsets.find(rowset->rowset_id());
    if (it == _unused_rowsets.end()) {
        _unused_rowsets[rowset->rowset_id()] = rowset;
    }
    _gc_mutex.unlock();
}

// TODO(zc): refactor this funciton
OLAPStatus StorageEngine::create_tablet(const TCreateTabletReq& request) {
    
    // Get all available stores, use ref_root_path if the caller specified
    std::vector<DataDir*> stores;
    stores = get_stores_for_create_tablet(request.storage_medium);
    if (stores.empty()) {
        LOG(WARNING) << "there is no available disk that can be used to create tablet.";
        return OLAP_ERR_CE_CMD_PARAMS_ERROR;
    }
    return TabletManager::instance()->create_tablet(request, stores);
}

OLAPStatus StorageEngine::recover_tablet_until_specfic_version(
        const TRecoverTabletReq& recover_tablet_req) {
    TabletSharedPtr tablet = TabletManager::instance()->get_tablet(recover_tablet_req.tablet_id,
                                   recover_tablet_req.schema_hash);
    if (tablet == nullptr) { return OLAP_ERR_TABLE_NOT_FOUND; }
    RETURN_NOT_OK(tablet->recover_tablet_until_specfic_version(recover_tablet_req.version,
                                                        recover_tablet_req.version_hash));
    return OLAP_SUCCESS;
}

OLAPStatus StorageEngine::obtain_shard_path(
        TStorageMedium::type storage_medium, std::string* shard_path, DataDir** store) {
    LOG(INFO) << "begin to process obtain root path. storage_medium=" << storage_medium;
    OLAPStatus res = OLAP_SUCCESS;

    if (shard_path == NULL) {
        OLAP_LOG_WARNING("invalid output parameter which is null pointer.");
        return OLAP_ERR_CE_CMD_PARAMS_ERROR;
    }

    auto stores = get_stores_for_create_tablet(storage_medium);
    if (stores.empty()) {
        OLAP_LOG_WARNING("no available disk can be used to create tablet.");
        return OLAP_ERR_NO_AVAILABLE_ROOT_PATH;
    }

    uint64_t shard = 0;
    res = stores[0]->get_shard(&shard);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to get root path shard. [res=%d]", res);
        return res;
    }

    stringstream root_path_stream;
    root_path_stream << stores[0]->path() << DATA_PREFIX << "/" << shard;
    *shard_path = root_path_stream.str();
    *store = stores[0];

    LOG(INFO) << "success to process obtain root path. path=" << shard_path;
    return res;
}

OLAPStatus StorageEngine::load_header(
        const string& shard_path,
        const TCloneReq& request) {
    LOG(INFO) << "begin to process load headers."
              << "tablet_id=" << request.tablet_id
              << ", schema_hash=" << request.schema_hash;
    OLAPStatus res = OLAP_SUCCESS;

    DataDir* store = nullptr;
    {
        // TODO(zc)
        try {
            auto store_path =
                boost::filesystem::path(shard_path).parent_path().parent_path().string();
            store = get_store(store_path);
            if (store == nullptr) {
                LOG(WARNING) << "invalid shard path, path=" << shard_path;
                return OLAP_ERR_INVALID_ROOT_PATH;
            }
        } catch (...) {
            LOG(WARNING) << "invalid shard path, path=" << shard_path;
            return OLAP_ERR_INVALID_ROOT_PATH;
        }
    }

    stringstream schema_hash_path_stream;
    schema_hash_path_stream << shard_path
                            << "/" << request.tablet_id
                            << "/" << request.schema_hash;
    res = TabletManager::instance()->load_one_tablet(
            store,
            request.tablet_id, request.schema_hash,
            schema_hash_path_stream.str(), false);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to process load headers. [res=%d]", res);
        return res;
    }

    LOG(INFO) << "success to process load headers.";
    return res;
}

OLAPStatus StorageEngine::load_header(
        DataDir* store,
        const string& shard_path,
        TTabletId tablet_id,
        TSchemaHash schema_hash) {
    LOG(INFO) << "begin to process load headers. tablet_id=" << tablet_id
              << "schema_hash=" << schema_hash;
    OLAPStatus res = OLAP_SUCCESS;

    stringstream schema_hash_path_stream;
    schema_hash_path_stream << shard_path
                            << "/" << tablet_id
                            << "/" << schema_hash;
    res =  TabletManager::instance()->load_one_tablet(
            store,
            tablet_id, schema_hash,
            schema_hash_path_stream.str(), 
            false);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to process load headers. [res=%d]", res);
        return res;
    }

    LOG(INFO) << "success to process load headers.";
    return res;
}


OLAPStatus StorageEngine::execute_task(EngineTask* task) {
    // 1. add wlock to related tablets
    // 2. do prepare work
    // 3. release wlock
    {
        vector<TabletInfo> tablet_infos;
        task->get_related_tablets(&tablet_infos);
        sort(tablet_infos.begin(), tablet_infos.end());
        vector<TabletSharedPtr> related_tablets;
        for (TabletInfo& tablet_info : tablet_infos) {
            TabletSharedPtr tablet = TabletManager::instance()->get_tablet(
                tablet_info.tablet_id, tablet_info.schema_hash, false);
            if (tablet != NULL) {
                related_tablets.push_back(tablet);
                tablet->obtain_header_wrlock();
            } else {
                LOG(WARNING) << "could not get tablet before prepare tabletid: " 
                             << tablet_info.tablet_id;
            }
        }
        // add write lock to all related tablets
        OLAPStatus prepare_status = task->prepare();
        for (TabletSharedPtr& tablet : related_tablets) {
            tablet->release_header_lock();
        }
        if (prepare_status != OLAP_SUCCESS) {
            return prepare_status;
        }
    }

    // do execute work without lock
    OLAPStatus exec_status = task->execute();
    if (exec_status != OLAP_SUCCESS) {
        return exec_status;
    }
    
    // 1. add wlock to related tablets
    // 2. do finish work
    // 3. release wlock
    {
        vector<TabletInfo> tablet_infos;
        // related tablets may be changed after execute task, so that get them here again
        task->get_related_tablets(&tablet_infos);
        sort(tablet_infos.begin(), tablet_infos.end());
        vector<TabletSharedPtr> related_tablets;
        for (TabletInfo& tablet_info : tablet_infos) {
            TabletSharedPtr tablet = TabletManager::instance()->get_tablet(
                tablet_info.tablet_id, tablet_info.schema_hash, false);
            if (tablet != NULL) {
                related_tablets.push_back(tablet);
                tablet->obtain_header_wrlock();
            } else {
                LOG(WARNING) << "could not get tablet before finish tabletid: " 
                             << tablet_info.tablet_id;
            }
        }
        // add write lock to all related tablets
        OLAPStatus fin_status = task->finish();
        for (TabletSharedPtr& tablet : related_tablets) {
            tablet->release_header_lock();
        }
        return fin_status;
    }
}

}  // namespace doris
